#include "td/actor/actor.h"
#include "td/net/UdpServer.h"
#include "td/utils/tests.h"

class PingPong : public td::actor::Actor {
 public:
  PingPong(int port, td::IPAddress dest, bool use_tcp, bool is_first)
      : port_(port), dest_(std::move(dest)), use_tcp_(use_tcp) {
    if (is_first) {
      state_ = Send;
      to_send_cnt_ = 5;
      to_send_cnt_ = 1;
    }
  }

 private:
  int port_;
  td::actor::ActorOwn<td::UdpServer> udp_server_;
  td::IPAddress dest_;
  bool is_closing_{false};
  bool use_tcp_{false};
  enum State { Send, Receive } state_{State::Receive};
  int cnt_{0};
  int to_send_cnt_{0};

  void start_up() override {
    class Callback : public td::UdpServer::Callback {
     public:
      Callback(td::actor::ActorShared<PingPong> ping_pong) : ping_pong_(std::move(ping_pong)) {
      }

     private:
      td::actor::ActorShared<PingPong> ping_pong_;
      void on_udp_message(td::UdpMessage udp_message) override {
        send_closure(ping_pong_, &PingPong::on_udp_message, std::move(udp_message));
      }
    };

    if (use_tcp_) {
      udp_server_ = td::UdpServer::create_via_tcp(PSLICE() << "UdpServer(via tcp) " << td::tag("port", port_), port_,
                                                  std::make_unique<Callback>(actor_shared(this)))
                        .move_as_ok();
    } else {
      udp_server_ = td::UdpServer::create(PSLICE() << "UdpServer " << td::tag("port", port_), port_,
                                          std::make_unique<Callback>(actor_shared(this)))
                        .move_as_ok();
    }

    alarm_timestamp() = td::Timestamp::in(0.1);
  }

  void on_udp_message(td::UdpMessage message) {
    if (is_closing_) {
      return;
    }
    if (message.error.is_error()) {
      LOG(ERROR) << "Got error " << message.error << " from " << message.address;
      return;
    }

    auto data_slice = message.data.as_slice();
    LOG(INFO) << "Got query " << td::format::escaped(data_slice) << " from " << message.address;
    CHECK(state_ == State::Receive);
    if (data_slice.size() < 5) {
      CHECK(data_slice == "stop");
      close();
    }
    if (data_slice[5] == 'i') {
      state_ = State::Send;
      to_send_cnt_ = td::Random::fast(1, 4);
      to_send_cnt_ = 1;
      send_closure_later(actor_id(this), &PingPong::loop);
    }
  }
  void loop() override {
    if (state_ != State::Send || is_closing_) {
      return;
    }
    to_send_cnt_--;
    td::Slice msg;
    if (to_send_cnt_ <= 0) {
      state_ = State::Receive;
      cnt_++;
      if (cnt_ >= 1000) {
        msg = "stop";
      } else {
        msg = "makgpingping";
      }
    } else {
      msg = "magkpongpong";
    }
    LOG(INFO) << "Send query: " << msg;
    send_closure_later(actor_id(this), &PingPong::loop);
    send_closure(udp_server_, &td::UdpServer::send, td::UdpMessage{dest_, td::BufferSlice(msg), {}});

    if (msg.size() == 4) {
      close();
    }
  }

  void alarm() override {
    send_closure_later(actor_id(this), &PingPong::loop);
  }

  void close() {
    is_closing_ = true;
    udp_server_.reset();
  }
  void hangup_shared() override {
    // udp_server_ was_closed
    stop();
  }
  void tear_down() override {
    td::actor::SchedulerContext::get()->stop();
  }
};

void run_server(int from_port, int to_port, bool is_first) {
  td::IPAddress to_ip;
  to_ip.init_host_port("localhost", to_port).ensure();

  td::actor::Scheduler scheduler({1});
  scheduler.run_in_context([&] {
    td::actor::create_actor<PingPong>(td::actor::ActorOptions().with_name("PingPong"), from_port, to_ip,
                                      false /*use_tcp*/, is_first)
        .release();
  });
  scheduler.run();
}

TEST(Net, PingPong) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(DEBUG));
  auto a = td::thread([] { run_server(8091, 8092, true); });
  auto b = td::thread([] { run_server(8092, 8091, false); });
  a.join();
  b.join();
}
