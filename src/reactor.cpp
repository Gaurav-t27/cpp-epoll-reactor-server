#include <iostream>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "reactor.hpp"

Reactor::Reactor() : m_epollFd(-1), m_shutdownFd(-1) {
    m_epollFd = epoll_create1(0);
    if (m_epollFd == -1) {
        throw std::runtime_error("Failed to create epoll instance");
    }

    // Create eventfd for shutdown signaling
    m_shutdownFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_shutdownFd == -1) {
        close(m_epollFd);
        throw std::runtime_error("Failed to create shutdown eventfd");
    }

    // Register shutdown fd with epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = m_shutdownFd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_shutdownFd, &ev) == -1) {
        close(m_shutdownFd);
        close(m_epollFd);
        throw std::runtime_error("Failed to register shutdown fd with epoll");
    }
}

Reactor::~Reactor() {
    m_handlers.clear();
    if (m_shutdownFd != -1) {
        close(m_shutdownFd);
    }
    if (m_epollFd != -1) {
        close(m_epollFd);
    }
}

void Reactor::registerHandler(int fd, uint32_t events, EventHandler handler) {
    if(m_handlers.find(fd) != m_handlers.end()) {
        throw std::runtime_error("Handler already registered for fd " + std::to_string(fd));
    }
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        if(errno == EEXIST) {
            if(epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) == -1) {
                std::cerr << "Warning: Failed to modify fd " << fd << " in epoll: " << std::strerror(errno) << std::endl;
                return;
            }
            // MOD succeeded — fall through to store the handler
        } else {
            std::cerr << "Warning: Failed to register fd " << fd << " with epoll: " << std::strerror(errno) << std::endl;
            return;
        }
    }
    m_handlers[fd] = std::move(handler);

}

void Reactor::unregisterHandler(int fd) {
    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        std::cerr << "Warning: Failed to unregister fd " << fd << " from epoll: " << std::strerror(errno) << std::endl;
        return;
    }
    m_handlers.erase(fd);
}

void Reactor::modifyHandler(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        std::cerr << "Warning: Failed to modify fd " << fd << " in epoll: " << std::strerror(errno) << std::endl;
    }
}

void Reactor::run() {
    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];
    bool running = true;

    while (running) {
        int nfds = epoll_wait(m_epollFd, events, MAX_EVENTS, -1); // Blocking wait
        if (nfds == -1) {
            if(errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            throw std::runtime_error("epoll_wait failed: " + std::string(std::strerror(errno)));
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            
            // Check if this is the shutdown signal
            if (fd == m_shutdownFd) {
                uint64_t val;
                read(m_shutdownFd, &val, sizeof(val)); // Drain the eventfd
                running = false;
                break;
            }
            
            auto it = m_handlers.find(fd);
            if (it != m_handlers.end()) {
                try {
                    it->second(fd, events[i].events);
                } catch (const std::exception& e) {
                    std::cerr << "Handler for fd " << fd << " threw: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Handler for fd " << fd << " threw an unknown exception" << std::endl;
                }
            }
        }
    }
}
