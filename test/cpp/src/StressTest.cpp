/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <thrift/concurrency/ThreadManager.h>
#include <thrift/concurrency/ThreadFactory.h>
#include <thrift/concurrency/Monitor.h>
#include <thrift/concurrency/Mutex.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TSimpleServer.h>
#include <thrift/server/TThreadPoolServer.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>
#include <thrift/transport/TFileTransport.h>
#include <thrift/TLogging.h>

#include "Service.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <sstream>
#include <map>
#if _WIN32
#include <thrift/windows/TWinsockSingleton.h>
#endif

using namespace std;

using namespace apache::thrift;
using namespace apache::thrift::async;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;
using namespace apache::thrift::concurrency;

using namespace test::stress;

struct eqstr {
  bool operator()(const char* s1, const char* s2) const { return strcmp(s1, s2) == 0; }
};

struct ltstr {
  bool operator()(const char* s1, const char* s2) const { return strcmp(s1, s2) < 0; }
};

// typedef hash_map<const char*, int, hash<const char*>, eqstr> count_map;
typedef map<const char*, int, ltstr> count_map;

class Server : public ServiceIf {
public:
  Server() = default;

  void count(const char* method) {
    Guard m(lock_);
    int ct = counts_[method];
    counts_[method] = ++ct;
  }

  void echoVoid() override {
    count("echoVoid");
    return;
  }

  count_map getCount() {
    Guard m(lock_);
    return counts_;
  }

  int8_t echoByte(const int8_t arg) override { return arg; }
  int32_t echoI32(const int32_t arg) override { return arg; }
  int64_t echoI64(const int64_t arg) override { return arg; }
  void echoString(string& out, const string& arg) override {
    if (arg != "hello") {
      T_ERROR_ABORT("WRONG STRING (%s)!!!!", arg.c_str());
    }
    out = arg;
  }
  void echoList(vector<int8_t>& out, const vector<int8_t>& arg) override { out = arg; }
  void echoSet(set<int8_t>& out, const set<int8_t>& arg) override { out = arg; }
  void echoMap(map<int8_t, int8_t>& out, const map<int8_t, int8_t>& arg) override { out = arg; }

private:
  count_map counts_;
  Mutex lock_;
};

enum TransportOpenCloseBehavior {
  OpenAndCloseTransportInThread,
  DontOpenAndCloseTransportInThread
};
class ClientThread : public Runnable {
public:
  ClientThread(std::shared_ptr<TTransport> transport,
               std::shared_ptr<ServiceIf> client,
               Monitor& monitor,
               size_t& workerCount,
               size_t loopCount,
               TType loopType,
               TransportOpenCloseBehavior behavior)
    : _transport(transport),
      _client(client),
      _monitor(monitor),
      _workerCount(workerCount),
      _loopCount(loopCount),
      _loopType(loopType),
      _behavior(behavior) {}

  void run() override {

    // Wait for all worker threads to start

    {
      Synchronized s(_monitor);
      while (_workerCount == 0) {
        _monitor.wait();
      }
    }

    _startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(_behavior == OpenAndCloseTransportInThread) {
      _transport->open();
    }

    switch (_loopType) {
    case T_VOID:
      loopEchoVoid();
      break;
    case T_BYTE:
      loopEchoByte();
      break;
    case T_I32:
      loopEchoI32();
      break;
    case T_I64:
      loopEchoI64();
      break;
    case T_STRING:
      loopEchoString();
      break;
    default:
      cerr << "Unexpected loop type" << _loopType << '\n';
      break;
    }

    _endTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if(_behavior == OpenAndCloseTransportInThread) {
      _transport->close();
    }

    _done = true;

    {
      Synchronized s(_monitor);

      _workerCount--;

      if (_workerCount == 0) {

        _monitor.notify();
      }
    }
  }

  void loopEchoVoid() {
    for (size_t ix = 0; ix < _loopCount; ix++) {
      _client->echoVoid();
    }
  }

  void loopEchoByte() {
    for (size_t ix = 0; ix < _loopCount; ix++) {
      int8_t arg = 1;
      int8_t result;
      result = _client->echoByte(arg);
      (void)result;
      assert(result == arg);
    }
  }

  void loopEchoI32() {
    for (size_t ix = 0; ix < _loopCount; ix++) {
      int32_t arg = 1;
      int32_t result;
      result = _client->echoI32(arg);
      (void)result;
      assert(result == arg);
    }
  }

  void loopEchoI64() {
    for (size_t ix = 0; ix < _loopCount; ix++) {
      int64_t arg = 1;
      int64_t result;
      result = _client->echoI64(arg);
      (void)result;
      assert(result == arg);
    }
  }

  void loopEchoString() {
    for (size_t ix = 0; ix < _loopCount; ix++) {
      string arg = "hello";
      string result;
      _client->echoString(result, arg);
      assert(result == arg);
    }
  }

  std::shared_ptr<TTransport> _transport;
  std::shared_ptr<ServiceIf> _client;
  Monitor& _monitor;
  size_t& _workerCount;
  size_t _loopCount;
  TType _loopType;
  int64_t _startTime;
  int64_t _endTime;
  bool _done;
  Monitor _sleep;
  TransportOpenCloseBehavior _behavior;
};

class TStartObserver : public apache::thrift::server::TServerEventHandler {
public:
  TStartObserver() : awake_(false) {}
  void preServe() override {
    apache::thrift::concurrency::Synchronized s(m_);
    awake_ = true;
    m_.notifyAll();
  }
  void waitForService() {
    apache::thrift::concurrency::Synchronized s(m_);
    while (!awake_)
      m_.waitForever();
  }

private:
  apache::thrift::concurrency::Monitor m_;
  bool awake_;
};

int main(int argc, char** argv) {
#if _WIN32
  transport::TWinsockSingleton::create();
#endif

  int port = 9091;
  string clientType = "regular";
  string serverType = "thread-pool";
  string protocolType = "binary";
  size_t workerCount = 8;
  size_t clientCount = 4;
  size_t loopCount = 50000;
  TType loopType = T_VOID;
  string callName = "echoVoid";
  bool runServer = true;
  bool logRequests = false;
  string requestLogPath = "./requestlog.tlog";
  bool replayRequests = false;

  ostringstream usage;

  usage << argv[0] << " [--port=<port number>] [--server] [--server-type=<server-type>] "
                      "[--protocol-type=<protocol-type>] [--workers=<worker-count>] "
                      "[--clients=<client-count>] [--loop=<loop-count>] "
                      "[--client-type=<client-type>]" << '\n'
        << "\tclients        Number of client threads to create - 0 implies no clients, i.e. "
                            "server only.  Default is " << clientCount << '\n'
        << "\thelp           Prints this help text." << '\n'
        << "\tcall           Service method to call.  Default is " << callName << '\n'
        << "\tloop           The number of remote thrift calls each client makes.  Default is " << loopCount << '\n'
        << "\tport           The port the server and clients should bind to "
                            "for thrift network connections.  Default is " << port << '\n'
        << "\tserver         Run the Thrift server in this process.  Default is " << runServer << '\n'
        << "\tserver-type    Type of server, \"simple\" or \"thread-pool\".  Default is " << serverType << '\n'
        << "\tprotocol-type  Type of protocol, \"binary\", \"ascii\", or \"xml\".  Default is " << protocolType << '\n'
        << "\tlog-request    Log all request to ./requestlog.tlog. Default is " << logRequests << '\n'
        << "\treplay-request Replay requests from log file (./requestlog.tlog) Default is " << replayRequests << '\n'
        << "\tworkers        Number of thread pools workers.  Only valid "
                            "for thread-pool server type.  Default is " << workerCount << '\n'
        << "\tclient-type    Type of client, \"regular\" or \"concurrent\".  Default is " << clientType << '\n'
        << '\n';

  map<string, string> args;

  for (int ix = 1; ix < argc; ix++) {

    string arg(argv[ix]);

    if (arg.compare(0, 2, "--") == 0) {

      size_t end = arg.find_first_of("=", 2);

      string key = string(arg, 2, end - 2);

      if (end != string::npos) {
        args[key] = string(arg, end + 1);
      } else {
        args[key] = "true";
      }
    } else {
      throw invalid_argument("Unexcepted command line token: " + arg);
    }
  }

  try {

    if (!args["clients"].empty()) {
      clientCount = atoi(args["clients"].c_str());
    }

    if (!args["help"].empty()) {
      cerr << usage.str();
      return 0;
    }

    if (!args["loop"].empty()) {
      loopCount = atoi(args["loop"].c_str());
    }

    if (!args["call"].empty()) {
      callName = args["call"];
    }

    if (!args["port"].empty()) {
      port = atoi(args["port"].c_str());
    }

    if (!args["server"].empty()) {
      runServer = args["server"] == "true";
    }

    if (!args["log-request"].empty()) {
      logRequests = args["log-request"] == "true";
    }

    if (!args["replay-request"].empty()) {
      replayRequests = args["replay-request"] == "true";
    }

    if (!args["server-type"].empty()) {
      serverType = args["server-type"];

      if (serverType == "simple") {

      } else if (serverType == "thread-pool") {

      } else if (serverType == "threaded") {

      } else {

        throw invalid_argument("Unknown server type " + serverType);
      }
    }
    if (!args["client-type"].empty()) {
      clientType = args["client-type"];

      if (clientType == "regular") {

      } else if (clientType == "concurrent") {

      } else {

        throw invalid_argument("Unknown client type " + clientType);
      }
    }
    if (!args["workers"].empty()) {
      workerCount = atoi(args["workers"].c_str());
    }

  } catch (std::exception& e) {
    cerr << e.what() << '\n';
    cerr << usage.str();
  }

  std::shared_ptr<ThreadFactory> threadFactory
      = std::shared_ptr<ThreadFactory>(new ThreadFactory());

  // Dispatcher
  std::shared_ptr<Server> serviceHandler(new Server());

  if (replayRequests) {
    std::shared_ptr<Server> serviceHandler(new Server());
    std::shared_ptr<ServiceProcessor> serviceProcessor(new ServiceProcessor(serviceHandler));

    // Transports
    std::shared_ptr<TFileTransport> fileTransport(new TFileTransport(requestLogPath));
    fileTransport->setChunkSize(2 * 1024 * 1024);
    fileTransport->setMaxEventSize(1024 * 16);
    fileTransport->seekToEnd();

    // Protocol Factory
    std::shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());

    TFileProcessor fileProcessor(serviceProcessor, protocolFactory, fileTransport);

    fileProcessor.process(0, true);
    exit(0);
  }

  if (runServer) {

    std::shared_ptr<ServiceProcessor> serviceProcessor(new ServiceProcessor(serviceHandler));

    // Transport
    std::shared_ptr<TServerSocket> serverSocket(new TServerSocket(port));

    // Transport Factory
    std::shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());

    // Protocol Factory
    std::shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());

    if (logRequests) {
      // initialize the log file
      std::shared_ptr<TFileTransport> fileTransport(new TFileTransport(requestLogPath));
      fileTransport->setChunkSize(2 * 1024 * 1024);
      fileTransport->setMaxEventSize(1024 * 16);

      transportFactory
          = std::shared_ptr<TTransportFactory>(new TPipedTransportFactory(fileTransport));
    }

    std::shared_ptr<TServer> server;

    if (serverType == "simple") {

      server.reset(
          new TSimpleServer(serviceProcessor, serverSocket, transportFactory, protocolFactory));

    } else if (serverType == "threaded") {

      server.reset(
          new TThreadedServer(serviceProcessor, serverSocket, transportFactory, protocolFactory));

    } else if (serverType == "thread-pool") {

      std::shared_ptr<ThreadManager> threadManager
          = ThreadManager::newSimpleThreadManager(workerCount);

      threadManager->threadFactory(threadFactory);
      threadManager->start();
      server.reset(new TThreadPoolServer(serviceProcessor,
                                         serverSocket,
                                         transportFactory,
                                         protocolFactory,
                                         threadManager));
    }

    std::shared_ptr<TStartObserver> observer(new TStartObserver);
    server->setServerEventHandler(observer);
    std::shared_ptr<Thread> serverThread = threadFactory->newThread(server);

    cerr << "Starting the server on port " << port << '\n';

    serverThread->start();
    observer->waitForService();

    // If we aren't running clients, just wait forever for external clients
    if (clientCount == 0) {
      serverThread->join();
    }
  }

  if (clientCount > 0) { //FIXME: start here for client type?

    Monitor monitor;

    size_t threadCount = 0;

    set<std::shared_ptr<Thread> > clientThreads;

    if (callName == "echoVoid") {
      loopType = T_VOID;
    } else if (callName == "echoByte") {
      loopType = T_BYTE;
    } else if (callName == "echoI32") {
      loopType = T_I32;
    } else if (callName == "echoI64") {
      loopType = T_I64;
    } else if (callName == "echoString") {
      loopType = T_STRING;
    } else {
      throw invalid_argument("Unknown service call " + callName);
    }

    if(clientType == "regular") {
      for (size_t ix = 0; ix < clientCount; ix++) {

        std::shared_ptr<TSocket> socket(new TSocket("127.0.0.1", port));
        std::shared_ptr<TBufferedTransport> bufferedSocket(new TBufferedTransport(socket, 2048));
        std::shared_ptr<TProtocol> protocol(new TBinaryProtocol(bufferedSocket));
        std::shared_ptr<ServiceClient> serviceClient(new ServiceClient(protocol));

        clientThreads.insert(threadFactory->newThread(std::shared_ptr<ClientThread>(
            new ClientThread(socket, serviceClient, monitor, threadCount, loopCount, loopType, OpenAndCloseTransportInThread))));
      }
    } else if(clientType == "concurrent") {
      std::shared_ptr<TSocket> socket(new TSocket("127.0.0.1", port));
      std::shared_ptr<TBufferedTransport> bufferedSocket(new TBufferedTransport(socket, 2048));
      std::shared_ptr<TProtocol> protocol(new TBinaryProtocol(bufferedSocket));
      auto sync = std::make_shared<TConcurrentClientSyncInfo>();
      std::shared_ptr<ServiceConcurrentClient> serviceClient(new ServiceConcurrentClient(protocol, sync));
      socket->open();
      for (size_t ix = 0; ix < clientCount; ix++) {
        clientThreads.insert(threadFactory->newThread(std::shared_ptr<ClientThread>(
            new ClientThread(socket, serviceClient, monitor, threadCount, loopCount, loopType, DontOpenAndCloseTransportInThread))));
      }
    }

    for (auto thread = clientThreads.begin();
         thread != clientThreads.end();
         thread++) {
      (*thread)->start();
    }

    int64_t time00;
    int64_t time01;

    {
      Synchronized s(monitor);
      threadCount = clientCount;

      cerr << "Launch " << clientCount << " " << clientType << " client threads" << '\n';

      time00 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

      monitor.notifyAll();

      while (threadCount > 0) {
        monitor.wait();
      }

      time01 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int64_t firstTime = 9223372036854775807LL;
    int64_t lastTime = 0;

    double averageTime = 0;
    int64_t minTime = 9223372036854775807LL;
    int64_t maxTime = 0;

    for (auto ix = clientThreads.begin();
         ix != clientThreads.end();
         ix++) {

      std::shared_ptr<ClientThread> client
          = std::dynamic_pointer_cast<ClientThread>((*ix)->runnable());

      int64_t delta = client->_endTime - client->_startTime;

      assert(delta > 0);

      if (client->_startTime < firstTime) {
        firstTime = client->_startTime;
      }

      if (client->_endTime > lastTime) {
        lastTime = client->_endTime;
      }

      if (delta < minTime) {
        minTime = delta;
      }

      if (delta > maxTime) {
        maxTime = delta;
      }

      averageTime += delta;
    }

    averageTime /= clientCount;

    cout << "workers :" << workerCount << ", client : " << clientCount << ", loops : " << loopCount
         << ", rate : " << (clientCount * loopCount * 1000) / ((double)(time01 - time00)) << '\n';

    count_map count = serviceHandler->getCount();
    count_map::iterator iter;
    for (iter = count.begin(); iter != count.end(); ++iter) {
      printf("%s => %d\n", iter->first, iter->second);
    }
    cerr << "done." << '\n';
  }

  return 0;
}
