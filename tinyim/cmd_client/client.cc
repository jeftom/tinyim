#include "access/access.pb.h"

#include <gflags/gflags.h>
#include <brpc/channel.h>
#include <bthread/bthread.h>
#include <brpc/stream.h>
#include <butil/logging.h>

#include "linenoise.h"
#include "type.h"
#include "util/initialize.h"


DEFINE_string(connection_type, "single", "Connection type. Available values: single, pooled, short");
DEFINE_string(server, "0.0.0.0:8001", "IP Address of server");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)");
DEFINE_int32(internal_send_heartbeat_s, 10, "Internal time that send heartbeat");

namespace tinyim {

void completion(const char *buf, linenoiseCompletions *lc) {
  if (buf[0] == 's') {
    linenoiseAddCompletion(lc,"sendmsgto");
  }
}

const char *hints(const char *buf, int *color, int *bold) {
  if (!strcasecmp(buf,"sendmsgto")) {
    *color = 35;
    *bold = 0;
    return " <userid> <msg>";
  }
  return NULL;
}

struct PullDataArgs {
  tinyim::UserId user_id;
  brpc::Channel* channel;
};

// run in bthread
void* PullData(void *arg){
  LOG(INFO) << "PullData bthread running";
  auto args = static_cast<PullDataArgs*>(arg);
  auto pchannel = args->channel;
  auto user_id = args->user_id;
  delete args;

  while (!brpc::IsAskedToQuit()) {
    brpc::Controller cntl;
    tinyim::Ping ping;
    ping.set_user_id(user_id);
    tinyim::PullReply pull_reply;
    tinyim::AccessService_Stub stub(pchannel);

    LOG(INFO) << "Calling PullData";
    cntl.set_timeout_ms(0x7fffffff);
    stub.PullData(&cntl, &ping, &pull_reply, nullptr);
    if (cntl.Failed()){
      LOG(ERROR) << "Fail to call PullData. " << cntl.ErrorText();
      return nullptr;
    }
    switch (pull_reply.data_type()){
      case tinyim::DataType::NONE:
        break;
      case tinyim::DataType::MESSAGE:
        if (pull_reply.has_msgs()){
          const auto msgs = pull_reply.msgs();
          const size_t msg_size = msgs.msg_size();
          LOG_IF(INFO, msg_size > 0) << "Received pull reply";
          for (size_t i = 0; i < msg_size; ++i){
            const tinyim::Msg& msg = msgs.msg(i);

            LOG(INFO) << "Received pull reply. userid=" << msg.user_id()
                      << " peer_id=" << msg.peer_id()
                      << " msgid=" << msg.msg_id()
                      << " msg_type=" << msg.msg_type()
                      << " message=" << msg.message()
                      << " timestamp=" << msg.timestamp();
          }
        }
        break;

      case tinyim::DataType::ADDUSER:
      case tinyim::DataType::REMOVEUSER:
      case tinyim::DataType::ADDGROUP:
      case tinyim::DataType::REMOVEGROUP:
      default:
        LOG(WARNING) << "pull_reply.data_type=" << pull_reply.data_type();
        break;

    }
  }
  return nullptr;
}

void* SendHeartBeat(void* arg){
  auto pchannel = static_cast<brpc::Channel*>(arg);
  static const uint64_t sleep_time_s = FLAGS_internal_send_heartbeat_s * 1000 * 1000;

  brpc::Controller cntl;
  tinyim::Ping ping;
  ping.set_user_id(user_id);
  tinyim::Pong pong;
  auto msg_last_msg_id = pong.msg_last_msg_id();
  tinyim::AccessService_Stub stub(pchannel);
  stub.HeartBeat(&cntl, &ping, &ping, nullptr);
  if (cntl.Failed()){
    LOG(ERROR) << "Fail to call heartbeat. " << cntl.ErrorText();
    return nullptr;
  }
  else{
    bthread_usleep(sleep_time_s);
  }


}

// class StreamReceiveHandler: public brpc::StreamInputHandler{
  // virtual int on_received_messages(brpc::StreamId id,
                                   // butil::IOBuf *const messages[],
                                   // size_t size) {
    // std::ostringstream os;
    // for (size_t i = 0; i < size; ++i) {
        // os << "msg[" << i << "]=" << *messages[i];
    // }
    // LOG(INFO) << "Received from Stream=" << id << ": " << os.str();
    // return 0;
  // }
  // virtual void on_idle_timeout(brpc::StreamId id) {
    // LOG(INFO) << "Stream=" << id << " has no data transmission for a while";
  // }

  // virtual void on_closed(brpc::StreamId id) {
    // LOG(INFO) << "Stream=" << id << " is closed";
    // // brpc::StreamClose(id);
  // }
// };

struct HeartBeatArg{
  brpc::Channel* channel;
  tinyim::UserId user_id;
  MsgId cur_user_id;
};

void HeartBeatTimeOutHeadler(void* arg){
  const auto parg = static_cast<HeartBeatArg*>(arg);
  parg->channel;
  brpc::Controller cntl;
  tinyim::Ping ping;
  ping.set_user_id(parg->user_id);
  tinyim::Pong pong;
  tinyim::AccessService_Stub stub(parg->channel);

  stub.HeartBeat(&cntl, &ping, &pong, nullptr);
  if (cntl.Failed()){
    LOG(ERROR) << "Fail to call heartbeat. " << cntl.ErrorText();
    return;
  }
  else{
    auto last_msg_id = pong.last_msg_id();
    if (last_msg_id > parg->cur_user_id){
      // TODO pull msg
    }

  }
}

// class Client{



 // private:
  // bthread_timer_t heartbeat_timeout_id_;

// }

}  // namespace tinyim

int main(int argc, char* argv[]) {
  tinyim::Initialize init(argc, &argv);

  char *line;
  char *prgname = argv[0];
  while(argc > 1) {
    argc--;
    argv++;
    if (!strcmp(*argv,"--multiline")) {
      linenoiseSetMultiLine(1);
      printf("Multi-line mode enabled.\n");
    } else if (!strcmp(*argv,"--keycodes")) {
      linenoisePrintKeyCodes();
      exit(0);
    } else {
      fprintf(stderr, "Usage: %s [--multiline] [--keycodes]\n", prgname);
      exit(1);
    }
  }
  linenoiseSetCompletionCallback(tinyim::completion);
  linenoiseSetHintsCallback(tinyim::hints);
  linenoiseHistoryLoad("history.txt");

  brpc::Channel channel;
  brpc::ChannelOptions options;
  options.protocol = brpc::PROTOCOL_BAIDU_STD;
  options.connection_type = FLAGS_connection_type;
  options.timeout_ms = FLAGS_timeout_ms/*milliseconds*/;
  options.max_retry = FLAGS_max_retry;
  if (channel.Init(FLAGS_server.c_str(), NULL) != 0) {
    LOG(ERROR) << "Fail to initialize channel";
    return -1;
  }

  // brpc::Controller stream_cntl;
  // brpc::StreamId stream;

  // tinyim::StreamReceiveHandler stream_receive_handler;
  // brpc::StreamOptions stream_options;
  // stream_options.handler = &stream_receive_handler;

  // if (brpc::StreamCreate(&stream, stream_cntl, &stream_options) != 0) {
      // LOG(ERROR) << "Fail to create stream";
      // return -1;
  // }
  // tinyim::AccessService_Stub stub(&channel);
  const tinyim::UserId user_id = 1234;

  auto pull_data_args = new tinyim::PullDataArgs{user_id, &channel};
  bthread_t pull_data_bt;
  bthread_start_background(&pull_data_bt, nullptr, tinyim::PullData, pull_data_args);
  // bthread_t heart_beat_bt;
  // bthread_start_background(&heart_beat_bt), nullptr, tinyim::SendHeartBeat, &channel);
  // tinyim::Pong tmp_pong;
  // tinyim::Ping ping;
  // ping.set_user_id(user_id);
  // stub.CreateStream(&stream_cntl, &ping, &tmp_pong, NULL);
  // if (stream_cntl.Failed()) {
      // LOG(ERROR) << "Fail to CreateStream, " << stream_cntl.ErrorText();
      // return -1;
  // }
  bthread_timer_t heartbeat_timeout_id;
  tinyim::HeartBeatArg heartbeatarg = {&channel, user_id};
  static const int64_t heartbeat_timeout_us = FLAGS_internal_send_heartbeat_s * 1000000;
  bthread_timer_add(&heartbeat_timeout_id,
                    butil::microseconds_to_timespec(heartbeat_timeout_us),
                    tinyim::HeartBeatTimeOutHeadler,
                    &heartbeatarg);

  // LOG(INFO) << "Created Stream=" << stream;
  // const tinyim::MsgId tmp_msg_id = 12345;
  const auto msg_type = tinyim::MsgType::SINGLE;

  size_t str_len = strlen("sendmsgto ");
  while (!brpc::IsAskedToQuit() && (line = linenoise("tinyim> ")) != nullptr) {
    const std::string lne(line);

    if (lne[0] != '\0' && lne[0] != '/' && lne.size() > str_len
        && !strncmp(lne.c_str(), "sendmsgto ", str_len)) {

      const tinyim::UserId peer_id = strtoll(line + str_len, nullptr, 10);
      auto offset = lne.find_last_of(' ');
      const std::string msg(lne.begin() + offset + 1, lne.end());

      tinyim::NewMsg new_msg;
      new_msg.set_user_id(user_id);
      new_msg.set_peer_id(peer_id);
      new_msg.set_message(msg);
      new_msg.set_msg_type(msg_type);
      // TODO time must be monotonically increasing
      new_msg.set_timestamp(std::time(nullptr));
      LOG(INFO) << "userid=" << user_id
                << " peer_id=" << peer_id
                << " msg_type=" << msg_type
                << " msg=" << msg
                << " timestamp=" << new_msg.timestamp();

      brpc::Controller cntl;
      tinyim::MsgReply msg_reply;
      tinyim::AccessService_Stub stub(&channel);

      bthread_timer_del(heartbeat_timeout_id);

      LOG(INFO) << "Calling SendMsg";
      // TODO should be async
      stub.SendMsg(&cntl, &new_msg, &msg_reply, nullptr);
      if (cntl.Failed()){
        LOG(ERROR) << "Fail to SendMsg, " << cntl.ErrorText();
        return -1;
      }
      LOG(INFO) << "Received msgreply. userid=" << msg_reply.user_id()
                << " msgid=" << msg_reply.msg_id()
                << " last_msg_id=" << msg_reply.last_msg_id()
                << " timestamp=" << msg_reply.timestamp();

      linenoiseHistoryAdd(line);
      linenoiseHistorySave("history.txt");
    } else if (!strncmp(line,"/historylen",11)) {
      int len = atoi(line+11);
      linenoiseHistorySetMaxLen(len);
    } else if (!strncmp(line, "/mask", 5)) {
      linenoiseMaskModeEnable();
    } else if (!strncmp(line, "/unmask", 7)) {
      linenoiseMaskModeDisable();
    } else if (line[0] == '/') {
      printf("Unreconized command: %s\n", line);
    }
    free(line);

    bthread_timer_add(&heartbeat_timeout_id,
                      butil::microseconds_to_timespec(heartbeat_timeout_us),
                      HeartBeatTimeOutHeadler,
                      &heartbeatarg);

  }

  LOG(INFO) << "access_client is going to quit";
  return 0;
}