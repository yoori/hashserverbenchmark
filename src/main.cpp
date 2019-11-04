#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <list>
#include <mutex>
#include <thread>

#include <boost/program_options.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/random.hpp>
#include <boost/timer.hpp>

namespace BAIp = boost::asio::ip;

/**
 * class RequestPool
 * contains preallocated messages for send and give it randomly for send threads
 */
class RequestPool
{
public:
  enum
  {
    UNIQUE_MESSAGE_COUNT = 1000
  };

  RequestPool(
    unsigned long min_message_size,
    unsigned long max_message_size,
    unsigned long requests_count_val);

  const std::string*
  get_request() throw();

  void
  processed_requests(unsigned long requests);

  void
  got_error(const std::string& error);

  bool
  wait_processing(std::string& error);

private:
  const unsigned long requests_count_;
  std::vector<std::string> messages_;

  mutable std::mutex lock_;
  std::condition_variable finished_;

  boost::random::mt19937 rng;
  boost::random::uniform_int_distribution<unsigned long> message_index_generator_;

  unsigned long available_requests_count_;
  unsigned long processed_requests_count_;
  std::unique_ptr<std::string> error_;
};

typedef std::shared_ptr<RequestPool> RequestPool_var;

/**
 * class Client
 * implement async requests sending and response reading
 */
class Client: public std::enable_shared_from_this<Client>
{
public:
  enum {
    MAX_PIPELINING_REQUESTS = 20,
    READ_BUF_SIZE = 4*1024
  };

  Client(
    const RequestPool_var& request_pool,
    boost::asio::io_service& io_service,
    const BAIp::address& address,
    short port);

  void
  activate();

protected:
  void
  order_write_();

  void
  order_read_();

  void
  handle_write_(const boost::system::error_code& error);

  void
  handle_read_(
    const boost::system::error_code& error,
    size_t bytes_transferred);

  void
  process_read_data_(size_t bytes_transferred);

private:
  RequestPool_var request_pool_;
  BAIp::tcp::endpoint endpoint_;
  BAIp::tcp::socket sock_;

  unsigned long send_requests_;
  unsigned long received_responses_;

  bool read_ordered_;
  char read_data_[READ_BUF_SIZE];

  bool write_ordered_;
};

typedef std::shared_ptr<Client> Client_var;

//
// RequestPool implementation
//
RequestPool::RequestPool(
  unsigned long min_message_size,
  unsigned long max_message_size,
  unsigned long requests_count)
  : requests_count_(requests_count),
    message_index_generator_(
      0,
      std::min(requests_count, static_cast<unsigned long>(UNIQUE_MESSAGE_COUNT)) - 1),
    available_requests_count_(requests_count),
    processed_requests_count_(0)
{
  unsigned long generate_requests = std::min(
    requests_count,
    static_cast<unsigned long>(UNIQUE_MESSAGE_COUNT));

  boost::random::mt19937 rng;
  boost::random::uniform_int_distribution<unsigned long> size_generator(
    min_message_size, max_message_size);
  boost::random::uniform_int_distribution<char> content_generator(
    32, 126);

  for(unsigned long i = 0; i < generate_requests; ++i)
  {
    unsigned long message_size = size_generator(rng);
    std::string message(message_size + 1, ' ');
    std::generate(message.begin(), --message.end(), std::bind(content_generator, rng));
    *message.rbegin() = '\n';
    messages_.emplace_back(std::move(message));
  }
}

const std::string*
RequestPool::get_request() throw()
{
  std::unique_lock<std::mutex> lock(lock_);
  if(available_requests_count_ == 0 || error_)
  {
    return 0;
  }

  --available_requests_count_;
  unsigned long mi = message_index_generator_(rng);
  assert(mi < messages_.size());
  return &messages_[mi];
}

void
RequestPool::processed_requests(unsigned long requests)
{
  bool signal_finished;

  {
    std::unique_lock<std::mutex> lock(lock_);
    processed_requests_count_ += requests;
    assert(processed_requests_count_ <= requests_count_);
      // problem can be here or in server responses
    signal_finished = processed_requests_count_ == requests_count_;
  }

  if(signal_finished)
  {
    finished_.notify_all();
  }
}

void
RequestPool::got_error(const std::string& error)
{
  {
    std::unique_lock<std::mutex> lock(lock_);
    error_.reset(new std::string(error));
  }

  finished_.notify_all();
}

bool
RequestPool::wait_processing(std::string& error)
{
  std::unique_lock<std::mutex> lock(lock_);

  while(processed_requests_count_ < requests_count_ && !error_)
  {
    finished_.wait(lock);
  }

  if(error_)
  {
    error = *error_;
    return false;
  }

  return true;
}

//
// Client implementation
//
Client::Client(
  const RequestPool_var& request_pool,
  boost::asio::io_service& io_service,
  const BAIp::address& address,
  short port)
  : request_pool_(request_pool),
    endpoint_(address, port),
    sock_(io_service),
    send_requests_(0),
    received_responses_(0),
    read_ordered_(false),
    write_ordered_(false)
{
  sock_.connect(endpoint_);
}

void
Client::activate()
{
  order_write_();
}

void
Client::order_write_()
{
  if(!write_ordered_ && send_requests_ - received_responses_ < MAX_PIPELINING_REQUESTS)
  {
    // choose message to send,
    // on last requests reading possible excess get_request calls
    const std::string* send_message = request_pool_->get_request();

    if(send_message)
    {
      boost::asio::async_write(
        sock_,
        boost::asio::buffer(send_message->data(), send_message->size()),
        boost::bind(
          &Client::handle_write_,
          shared_from_this(),
          boost::asio::placeholders::error));

      ++send_requests_;
      write_ordered_ = true;
    }
  }
}

void
Client::order_read_()
{
  if(!read_ordered_)
  {
    sock_.async_read_some(
      boost::asio::buffer(read_data_, sizeof(read_data_)),
      boost::bind(
        &Client::handle_read_,
        shared_from_this(),
        boost::asio::placeholders::error,
        boost::asio::placeholders::bytes_transferred));
    read_ordered_ = true;
  }
}

void
Client::handle_write_(const boost::system::error_code& error)
{
  if(!error)
  {
    write_ordered_ = false;

    order_read_();
    order_write_();
  }
  else
  {
    request_pool_->got_error(error.message());
  }
}

void
Client::handle_read_(
  const boost::system::error_code& error,
  size_t bytes_transferred)
{
  if(!error)
  {
    // read done
    read_ordered_ = false;

    process_read_data_(bytes_transferred);

    order_read_();
    order_write_();
  }
  else
  {
    request_pool_->got_error(error.message());
  }
}

void
Client::process_read_data_(size_t bytes_transferred)
{
  // calculate number of got responses without content check
  // this isn't functional test
  const char* data_start = read_data_;
  const char* data_end = read_data_ + bytes_transferred;
  while(true)
  {
    const char* response_end = std::find(data_start, data_end, '\n');
    if(response_end != data_end)
    {
      ++received_responses_;
      request_pool_->processed_requests(1);
      data_start = response_end + 1;
    }
    else
    {
      break;
    }
  }
}

// main
int
main(int argc, char* argv[])
{
  try
  {
    short concurrency = 1;
    std::string connect_address;
    short connect_port;
    unsigned long send_requests = 10000;
    unsigned long min_request_size = 10;
    unsigned long max_request_size = 10000;

    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
      ("help,h", "print usage message")
      ("concurrency,c", boost::program_options::value<short>(&concurrency),
        "number of threads for send messages")
      ("host,H", boost::program_options::value<std::string>(&connect_address)->required(),
        "connect ip")
      ("port,p", boost::program_options::value<short>(&connect_port)->required(),
        "connect port")
      ("requests,r", boost::program_options::value<unsigned long>(&send_requests),
        "number of requests to send")
      ("min-rs", boost::program_options::value<unsigned long>(&min_request_size),
        "min request size")
      ("max-rs", boost::program_options::value<unsigned long>(&max_request_size),
        "max request size")
      ;

    boost::program_options::variables_map vm;
    boost::program_options::store(
      boost::program_options::command_line_parser(argc, argv).options(desc).run(),
      vm);
    boost::program_options::notify(vm); // store values into variables

    // init RequestPool
    RequestPool_var request_pool(
      new RequestPool(min_request_size, max_request_size, send_requests));

    std::list<std::pair<std::unique_ptr<boost::asio::io_service>, Client_var> > clients;

    // create clients in connected state
    for(int i = 0; i < concurrency; ++i)
    {
      std::unique_ptr<boost::asio::io_service> io_service_ptr(
        new boost::asio::io_service());
      clients.emplace_back(
        std::move(io_service_ptr),
        new Client(
          request_pool,
          *io_service_ptr,
          BAIp::address::from_string(connect_address),
          connect_port));
    }

    // activate clients before io_service::run (it stop immediatly if here no registered handlers ...)
    auto start_time = std::chrono::high_resolution_clock::now();

    for(auto it = clients.begin(); it != clients.end(); ++it)
    {
      it->second->activate();
    }

    // call io_service::run (used separate io_service for each client)
    std::list<std::thread> threads;
    for(auto it = clients.begin(); it != clients.end(); ++it)
    {
      threads.push_back(
        std::thread(boost::bind(&boost::asio::io_service::run, it->first.get())));
    }

    std::string error;
    bool successfully_finished = request_pool->wait_processing(error);
    auto finish_time = std::chrono::high_resolution_clock::now();

    for(auto it = clients.begin(); it != clients.end(); ++it)
    {
      it->first->stop();
    }

    for(auto it = threads.begin(); it != threads.end(); ++it)
    {
      it->join();
    }

    if(successfully_finished)
    {
      long duration_ns =
        std::chrono::duration_cast<std::chrono::microseconds>(
          finish_time - start_time).count();
      
      std::cout << "avg request processing time = " <<
        std::setprecision(10) <<
        std::fixed <<
        (duration_ns / 1000000. / send_requests) <<
        std::endl <<
        "rps = ";
      if(duration_ns > 0)
      {
        std::cout << (send_requests * 1000000. / duration_ns) << std::endl;
      }
      else
      {
        std::cout << "very fast";
      }

      std::cout << std::endl;
    }
    else
    {
      std::cerr << "error: " << error << std::endl;
      return 1;
    }
  }
  catch (const std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
