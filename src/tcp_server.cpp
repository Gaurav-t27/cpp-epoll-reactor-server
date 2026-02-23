#include <iostream>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include "tcp_server.hpp"


TCPServer::TCPServer(int port): m_reactor(){
    m_listen_socket.setReuseAddr();
    m_listen_socket.setNonBlocking();
    m_listen_socket.bind(port);
    m_listen_socket.listen();

    m_reactor.registerHandler(m_listen_socket.getFd(), EPOLLIN|EPOLLET, [this](int fd, uint32_t events) {
        handleNewConnection(fd);
        (void)events; // Unused
    });
}

void TCPServer::start() {
    m_reactor.run();
}

int TCPServer::getPort() const {
    return m_listen_socket.getPort();
}

void TCPServer::handleNewConnection(int fd) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    while(true) {
        int client_fd = accept4(fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more incoming connections
                break;
            } else {
                std::cerr << "Failed to accept new connection: " << std::strerror(errno) << std::endl;
                break;
            }
        }

        std::unique_ptr<Socket> client_socket = std::make_unique<Socket>(client_fd);
        
        ClientState state;
        state.socket = std::move(client_socket);
        m_clients[client_fd] = std::move(state);

        m_reactor.registerHandler(client_fd, EPOLLIN|EPOLLRDHUP|EPOLLET, [this](int cfd, uint32_t events) {
            auto it = m_clients.find(cfd);
            if (it == m_clients.end()) return;

            if(events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                std::cerr << "Client fd " << cfd << " closed or error occurred" << std::endl;
                cleanupClient(cfd);
                return;
            }

            // If we have buffered data, try to write it first
            if (events & EPOLLOUT) {
                handleClientWrite(cfd);
            }

            // Then handle any incoming data
            if (events & EPOLLIN) {
                handleClientData(cfd);
            }
            
        });

        std::cout << "Accepted new connection, fd: " << client_fd << '\n';
    }
}

void TCPServer::handleClientData(int fd) {
    auto it = m_clients.find(fd);
    if (it == m_clients.end()) return;
    
    // Check if write buffer is above threshold - stop reading if so
    // for handling any queued(stale) EPOLLIN event in epoll, before EPOLLOUT was set
    if (it->second.write_buffer.size() >= MAX_WRITE_BUFFER_SIZE) {
        std::cout << "Write buffer full (" << it->second.write_buffer.size() 
                  << " bytes), pausing reads for fd " << fd << std::endl;
        m_reactor.modifyHandler(fd, EPOLLOUT | EPOLLET);
        return;
    }
    
    char temp_buf[4096];
    bool read_complete = false;
    
    // Drain all available data from socket
    while (true) {
        ssize_t bytes_read = read(fd, temp_buf, sizeof(temp_buf));
        
        if (bytes_read == 0) {
            // Clean client disconnect
            std::cout << "Client disconnected cleanly, fd: " << fd << std::endl;
            cleanupClient(fd);
            return;
        } else if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // All data read, done
                read_complete = true;
                break;
            }
            std::cerr << "Read error on fd " << fd << ": " << std::strerror(errno) << std::endl;
            cleanupClient(fd);
            return;
        }

        std::cout << "Received " << bytes_read << " bytes from fd " << fd << std::endl;
 
        std::transform(temp_buf, temp_buf + bytes_read, std::back_inserter(it->second.write_buffer), [](unsigned char c) {
            return static_cast<unsigned char>(std::toupper(c)); 
        });
        
        // Check if we've exceeded the buffer threshold after this read
        if (it->second.write_buffer.size() >= MAX_WRITE_BUFFER_SIZE) {
            std::cout << "Write buffer reached threshold (" << it->second.write_buffer.size() 
                      << " bytes), pausing reads for fd " << fd << std::endl;
            // Stop reading, only wait for EPOLLOUT to drain buffer
            m_reactor.modifyHandler(fd, EPOLLOUT | EPOLLET);
            handleClientWrite(fd);
            return;
        }
    }

    if(read_complete && !it->second.write_buffer.empty()) {
        handleClientWrite(fd);
    };
}

void TCPServer::handleClientWrite(int fd) {
    auto it = m_clients.find(fd);
    if (it == m_clients.end() || it->second.write_buffer.empty()) 
    {
        m_reactor.modifyHandler(fd, EPOLLIN | EPOLLET);
        return;
    }
    
    auto& buffer = it->second.write_buffer;
    
    // Try to flush buffered data
    while (!buffer.empty()) {
        ssize_t bytes_written = write(fd, buffer.data(), buffer.size());
        
        if (bytes_written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Check if we should resume reads despite having data left
                if (buffer.size() < RESUME_WRITE_BUFFER_SIZE) {
                    m_reactor.modifyHandler(fd, EPOLLIN | EPOLLOUT | EPOLLET);
                } else {
                    m_reactor.modifyHandler(fd, EPOLLOUT | EPOLLET);
                }
                return;
            } else if (errno == EPIPE || errno == ECONNRESET) {
                std::cerr << "Client disconnected during buffered write, fd: " << fd << std::endl;
                cleanupClient(fd);
                return;
            } else {
                std::cerr << "Write error on fd " << fd << ": " << std::strerror(errno) << std::endl;
                cleanupClient(fd);
                return;
            }
        }
        
        // Remove written data from buffer
        buffer.erase(buffer.begin(), buffer.begin() + bytes_written);
    }
    
    // Buffer is empty, resume reading
    std::cout << "Flushed write buffer for fd " << fd << std::endl;
    m_reactor.modifyHandler(fd, EPOLLIN | EPOLLET);
}

void TCPServer::cleanupClient(int fd) {
    m_reactor.unregisterHandler(fd);
    m_clients.erase(fd);
}
