#include <algorithm>
#include <iostream>

#include "http/server.hpp"

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    net::http::server_config config{};
    config.router.add().use(
        [](const net::http::server_request& req, net::http::response_writer& resp)
        {
            resp.headers().set("X-Msg"sv, "Hello"sv);
            resp.send(net::http::status::OK, 11).write("hello world", 11);
            std::cout << "handled: " << net::http::method_string(req.method) << " " << req.uri.build() << '\n';
        });
    config.logger->set_level(spdlog::level::trace);

    net::http::server server{config};

    std::cout << "starting...\n";
    server.serve();
    std::cout << "exiting...\n";

    return 0;
}

/* #include <chrono> */
/* #include <coroutine> */
/* #include <exception> */
/* #include <future> */
/* #include <iostream> */
/* #include <thread> */
/* #include <type_traits> */

/* using namespace std::chrono_literals; */

/* // A program-defined type on which the coroutine_traits specializations below depend */
/* struct as_coroutine */
/* {}; */

/* // Enable the use of std::future<T> as a coroutine type */
/* // by using a std::promise<T> as the promise type. */
/* template<typename T, typename... Args> */
/*     requires(!std::is_void_v<T> && !std::is_reference_v<T>) */
/* struct std::coroutine_traits<std::future<T>, as_coroutine, Args...> */
/* { */
/*     struct promise_type : std::promise<T> */
/*     { */
/*         std::future<T> get_return_object() noexcept { return this->get_future(); } */

/*         std::suspend_never initial_suspend() const noexcept { return {}; } */
/*         std::suspend_never final_suspend() const noexcept { return {}; } */

/*         void return_value(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) { this->set_value(value);
 * } */
/*         void return_value(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) */
/*         { */
/*             this->set_value(std::move(value)); */
/*         } */
/*         void unhandled_exception() noexcept { this->set_exception(std::current_exception()); } */
/*     }; */
/* }; */

/* // Allow co_await'ing std::future<T> and std::future<void> */
/* // by naively spawning a new thread for each co_await. */
/* template<typename T> */
/*     requires(!std::is_reference_v<T>) */
/* auto operator co_await(std::future<T> future) noexcept */
/* { */
/*     struct awaiter : std::future<T> */
/*     { */
/*         bool await_ready() const noexcept */
/*         { */
/*             std::cout << std::this_thread::get_id() << " await_ready()\n"; */
/*             return this->wait_for(1s) != std::future_status::timeout; */
/*         } */

/*         void await_suspend(std::coroutine_handle<> cont) const */
/*         { */
/*             std::cout << std::this_thread::get_id() << " await_suspend()\n"; */
/*             std::thread( */
/*                 [this, cont] */
/*                 { */
/*                     std::cout << std::this_thread::get_id() << " begin this->wait()\n"; */
/*                     this->wait(); */
/*                     std::cout << std::this_thread::get_id() << " finish this->wait()\n"; */

/*                     std::cout << std::this_thread::get_id() << " begin cont()\n"; */
/*                     cont(); */
/*                     std::cout << std::this_thread::get_id() << " finish cont()\n"; */
/*                 }) */
/*                 .detach(); */
/*         } */

/*         T await_resume() */
/*         { */
/*             std::cout << std::this_thread::get_id() << " await_resume()\n"; */
/*             return this->get(); */
/*         } */
/*     }; */
/*     return awaiter{std::move(future)}; */
/* } */

/* // Utilize the infrastructure we have established. */
/* std::future<int> compute(as_coroutine) */
/* { */
/*     int a = co_await std::async( */
/*         [] */
/*         { */
/*             std::cout << "a = " << std::this_thread::get_id() << "\n"; */
/*             std::this_thread::sleep_for(2s); */
/*             return 6; */
/*         }); */
/*     int b = co_await std::async( */
/*         [] */
/*         { */
/*             std::cout << "b = " << std::this_thread::get_id() << "\n"; */
/*             std::this_thread::sleep_for(1s); */
/*             return 7; */
/*         }); */

/*     co_return a* b; */
/* } */

/* int main() */
/* { */
/*     std::cout << "main begin = " << std::this_thread::get_id() << "\n"; */

/*     auto result = compute({}).get(); */

/*     std::cout << "main finish = " << std::this_thread::get_id() << "\n"; */

/*     std::cout << result << '\n'; */
/* } */
