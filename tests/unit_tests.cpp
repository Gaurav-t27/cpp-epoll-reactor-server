#include <cstring>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdexcept>
#include <catch2/catch_test_macros.hpp>
#include "../include/socket.hpp"
#include "../include/reactor.hpp"


// Test Socket RAII wrapper
TEST_CASE("Socket RAII basics", "[socket]") {
    
    SECTION("Socket creation and automatic cleanup") {
        {
            Socket s(socket(AF_INET, SOCK_STREAM, 0));
            REQUIRE(s.getFd() >= 0);
        }
        // Socket should be closed automatically when out of scope
    }
    
    SECTION("Move semantics work correctly") {
        Socket s1(socket(AF_INET, SOCK_STREAM, 0));
        int original_fd = s1.getFd();
        REQUIRE(original_fd >= 0);
        
        Socket s2(std::move(s1));
        REQUIRE(s1.getFd() == -1);  // s1 should be invalidated
        REQUIRE(s2.getFd() == original_fd);  // s2 should own the fd
    }
    
    SECTION("Move assignment works correctly") {
        Socket s1(socket(AF_INET, SOCK_STREAM, 0));
        int original_fd = s1.getFd();
        
        Socket s2;
        s2 = std::move(s1);
        
        REQUIRE(s1.getFd() == -1);
        REQUIRE(s2.getFd() == original_fd);
    }
}

TEST_CASE("Socket operations", "[socket]") {
    SECTION("Set non-blocking mode") {
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        REQUIRE(s.getFd() >= 0);
        
        REQUIRE_NOTHROW(s.setNonBlocking());
        
        // Verify non-blocking flag is set
        int flags = fcntl(s.getFd(), F_GETFL, 0);
        REQUIRE((flags & O_NONBLOCK) != 0);
    }
    
    SECTION("Set reuse address") {
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        REQUIRE(s.getFd() >= 0);
        
        REQUIRE_NOTHROW(s.setReuseAddr());
        
        // Verify SO_REUSEADDR is set
        int optval = 0;
        socklen_t optlen = sizeof(optval);
        getsockopt(s.getFd(), SOL_SOCKET, SO_REUSEADDR, &optval, &optlen);
        REQUIRE(optval == 1);
    }
    
}

TEST_CASE("Reactor basic operations", "[reactor]") {
    
    SECTION("Register and unregister handler") {
        Reactor reactor;
        
        // Create a test socket
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        REQUIRE(s.getFd() >= 0);
        
        int fd = s.getFd();
        bool handler_called = false;
        
        // Register handler
        REQUIRE_NOTHROW(reactor.registerHandler(fd, EPOLLIN, [&](int, uint32_t) {
            handler_called = true;
        }));
        
        // Unregister handler
        REQUIRE_NOTHROW(reactor.unregisterHandler(fd));
    }
    
    SECTION("Cannot register same fd twice") {
        Reactor reactor;
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        int fd = s.getFd();
        
        reactor.registerHandler(fd, EPOLLIN, [](int, uint32_t) {});
        
        // Second registration should throw
        REQUIRE_THROWS(reactor.registerHandler(fd, EPOLLIN, [](int, uint32_t) {}));
        
        reactor.unregisterHandler(fd);
    }
    
    SECTION("Modify handler event mask") {
        Reactor reactor;
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        int fd = s.getFd();
        
        reactor.registerHandler(fd, EPOLLIN, [](int, uint32_t) {});
        
        // Modify to add EPOLLOUT
        REQUIRE_NOTHROW(reactor.modifyHandler(fd, EPOLLIN | EPOLLOUT));
        
        reactor.unregisterHandler(fd);
    }
    
}

TEST_CASE("Reactor shutdown mechanism", "[reactor]") {
    SECTION("Shutdown fd is valid") {
        Reactor reactor;
        int shutdown_fd = reactor.getShutdownFd();
        REQUIRE(shutdown_fd >= 0);
        
        // Verify it's an eventfd
        uint64_t value = 1;
        ssize_t written = write(shutdown_fd, &value, sizeof(value));
        REQUIRE(written == sizeof(value));
    }
}


TEST_CASE("Reactor event handling with socket pair", "[reactor]") {
    SECTION("Handler is called when data available") {
        Reactor reactor;

        int sv[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

        Socket s1(sv[0]);
        Socket s2(sv[1]);
        s1.setNonBlocking();
        s2.setNonBlocking();

        bool handler_called = false;
        uint32_t events_received = 0;

        reactor.registerHandler(s2.getFd(), EPOLLIN, [&](int fd, uint32_t events) {
            handler_called = true;
            events_received = events;

            char buffer[100];
            read(fd, buffer, sizeof(buffer));

            uint64_t value = 1;
            write(reactor.getShutdownFd(), &value, sizeof(value));
        });

        const char* msg = "trigger";
        write(s1.getFd(), msg, strlen(msg));

        reactor.run();

        REQUIRE(handler_called);
        REQUIRE((events_received & EPOLLIN) != 0);

        reactor.unregisterHandler(s2.getFd());
    }

    SECTION("EPOLLRDHUP fires when peer closes write half") {
        Reactor reactor;

        int sv[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

        Socket s1(sv[0]);
        Socket s2(sv[1]);
        s1.setNonBlocking();
        s2.setNonBlocking();

        bool rdhup_received = false;

        reactor.registerHandler(s2.getFd(), EPOLLIN | EPOLLRDHUP, [&](int, uint32_t events) {
            rdhup_received = (events & EPOLLRDHUP) != 0;
            uint64_t value = 1;
            write(reactor.getShutdownFd(), &value, sizeof(value));
        });

        // Closing the write half of s1 sends a FIN, triggering EPOLLRDHUP on s2
        shutdown(sv[0], SHUT_WR);

        reactor.run();

        REQUIRE(rdhup_received);
        reactor.unregisterHandler(s2.getFd());
    }
}

TEST_CASE("Reactor error handling", "[reactor]") {
    SECTION("Reactor continues running after a handler throws") {
        Reactor reactor;

        int sv[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

        Socket s1(sv[0]);
        Socket s2(sv[1]);
        s1.setNonBlocking();
        s2.setNonBlocking();

        int call_count = 0;

        // Level-triggered EPOLLIN: if data remains after first read, epoll fires again.
        // Handler reads one byte at a time so "ab" produces two separate events.
        reactor.registerHandler(s2.getFd(), EPOLLIN, [&](int fd, uint32_t) {
            char buf[1];
            read(fd, buf, 1);
            ++call_count;
            if (call_count == 1) {
                throw std::runtime_error("deliberate handler error");
            }
            uint64_t value = 1;
            write(reactor.getShutdownFd(), &value, sizeof(value));
        });

        write(sv[0], "ab", 2);

        reactor.run();

        REQUIRE(call_count == 2);
        reactor.unregisterHandler(s2.getFd());
    }

    SECTION("Unregistering a non-existent handler does not throw") {
        Reactor reactor;
        Socket s(socket(AF_INET, SOCK_STREAM, 0));
        // Never registered — should not throw, just warn
        REQUIRE_NOTHROW(reactor.unregisterHandler(s.getFd()));
    }
}


