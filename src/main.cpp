#include "tcp.hpp"

#include <iostream>

int main(int argc, char** argv) try
{
    net::tcp_socket sock{"127.0.0.1", "9090"};

    struct foo
    {
        int32_t foo;
        int32_t bar;
        int32_t baz;
    };

    foo f{
        .foo = 5,
        .bar = 3,
        .baz = 7,
    };

    std::cerr << "sending...";
    sock << f;
    std::cerr << "done\n";

    std::cerr << "sent: foo{ foo: " << f.foo << ", bar: " << f.bar << ", baz: " << f.baz << " }\n";

    std::cerr << "reading...";
    sock >> f;
    std::cerr << "done\n";

    std::cerr << "received: foo{ foo: " << f.foo << ", bar: " << f.bar << ", baz: " << f.baz << " }\n";

    return 0;
}
catch(const std::exception& e)
{
    std::cerr << e.what() << std::endl;
    return 1;
}

