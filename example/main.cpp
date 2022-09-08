#include <iostream>

#include "http/http.hpp"
#include "http/router.hpp"

int main(int argc, char** argv)
{
    net::http::router router;

    /* router.add(net::http::method::GET, */
    /*            "/", */
    /*            [](const net::http::request& req, net::http::response& resp) { std::cout << "handled\n"; }); */

    return 0;
}
