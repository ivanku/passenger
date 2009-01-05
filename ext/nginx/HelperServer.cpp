#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <set>
#include <vector>
#include <string>

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include "oxt/thread.hpp"
#include "oxt/system_calls.hpp"

#include "ScgiRequestParser.h"
#include "HttpStatusExtractor.h"

#include "StandardApplicationPool.h"
#include "ApplicationPoolStatusReporter.h"
#include "Application.h"
#include "PoolOptions.h"
#include "Exceptions.h"
#include "Utils.h"

using namespace boost;
using namespace oxt;
using namespace Passenger;

#define HELPER_SERVER_PASSWORD_SIZE     64


/**
 * Wrapper class around a file descriptor integer, for RAII behavior.
 *
 * A FileDescriptor object behaves just like an int, so that you can pass it to
 * system calls such as read(). It performs reference counting. When the last
 * copy of a FileDescriptor has been destroyed, the underlying file descriptor
 * will be automatically closed.
 */
class FileDescriptor {
private:
	struct SharedData {
		int fd;
		
		/**
		 * Constructor to assign this file descriptor's handle.
		 */
		SharedData(int fd) {
			this->fd = fd;
		}
		
		/**
		 * Attempts to close this file descriptor. When created on the stack,
		 * this destructor will automatically be invoked as a result of C++
		 * semantics when exiting the scope this object was created in. This
		 * ensures that stack created objects with destructors like these will
		 * de-allocate their resources upon leaving their corresponding scope.
		 * This pattern is also known Resource Acquisition Is Initialization (RAII).
		 *
		 * @throws SystemException File descriptor could not be closed.
		 */
		~SharedData() {
			if (syscalls::close(fd) == -1) {
				throw SystemException("Cannot close file descriptor", errno);
			}
		}
	};
	
	/* Shared pointer for reference counting on this file descriptor */
	shared_ptr<SharedData> data;
	
public:
	FileDescriptor() {
		// Do nothing.
	}
	
	/**
	 * Creates a new FileDescriptor instance with the given fd as a handle.
	 */
	FileDescriptor(int fd) {
		data = ptr(new SharedData(fd));
	}
	
	/**
	 * Overloads the integer cast operator so that it will return the file
	 * descriptor handle as an integer.
	 *
	 * @return This file descriptor's handle as an integer.
	 */
	operator int () const {
		return data->fd;
	}
};

/**
 * A representation of a Client from the Server's point of view. This class
 * contains the methods used to communicate from a server to a connected
 * client, i.e. it is a client handler.
 * These Client instances will communicate concurrently with the server through
 * threads. Considering the overhead of these threads, i.e. setup and teardown
 * costs and the volatility of client requests, these client instances will be
 * pooled. It is for this reason that the State design pattern has been applied:
 * this class can be considered as a skeleton implemention whose state --e.g.
 * client file descriptor-- needs to be provided in order to function properly.
 */
class Client {
private:
	/** The client thread stack size in bytes. */
	static const int CLIENT_THREAD_STACK_SIZE = 1024 * 128;
	
	/** The client number for this Client object, assigned by Server. */
	unsigned int number;
	
	/** The application pool to which this Client object belongs to. */
	StandardApplicationPoolPtr pool;
	
	/* This clients password. */
	string password;
	
	/* The server socket file descriptor. */
	int serverSocket;
	
	/* This client thread. */
	oxt::thread *thr;
	
	/**
	 * Attempts to accept a connection made by the client.
	 *
	 * @return The file descriptor corresponding to the accepted connection.
	 * @throws SystemException Could not accept new connection.
	 */
	FileDescriptor acceptConnection() {
		TRACE_POINT();
		struct sockaddr_un addr;
		socklen_t addrlen = sizeof(addr);
		int fd = syscalls::accept(serverSocket,
			(struct sockaddr *) &addr,
			&addrlen);
		if (fd == -1) {
			throw SystemException("Cannot accept new connection", errno);
		} else {
			return FileDescriptor(fd);
		}
	}
	
	/**
	 * Reads and checks the password of a client message channel identified by the given file descriptor.
	 * The HelperServer makes extensive use of Unix Sockets that would normally allow other processes to
	 * connect to it as well. In our case, we just want to limit this to Nginx and it is for this reason
	 * that we've secured communication channels between this server and its clients with passwords.
	 * This method indicates whether or not the password of this client channel matches the one known to
	 * the server.
	 * 
	 * @param fd The file descriptor identifying the client message channel.
	 * @return True if the password of the client channel indicated by the given file descriptor
	 *   matches the password known to the server. False will be returned if either the
	 *   passwords don't match or EOF has been encountered.
	 */
	bool readAndCheckPassword(FileDescriptor &fd) {
		TRACE_POINT();
		MessageChannel channel(fd);
		char buf[HELPER_SERVER_PASSWORD_SIZE];
		
		if (channel.readRaw(buf, sizeof(buf))) {
			const char *password_data;
			
			password_data = const_cast<const string &>(password).c_str();
			return memcmp(password_data, buf, sizeof(buf)) == 0;
		} else {
			return false;
		}
	}
	
	/**
	 * Reads and parses the request headers from the given file descriptor with the given SCGI request parser
	 * and if succesful, assigns the remainder of the request (i.e. non SCGI header data) to the given 
	 * requestBody.
	 *
	 * @param fd The file descriptor to read and parse from.
	 * @param parser The ScgiRequestParser to use for parsing the SCGI headers.
	 * @param requestBody The requestBody that was extracted as a result from parsing the SCGI headers.
	 * @return True if the request was succesfully read and parsed. False if an invalid SCGI header was
	 *   received by the parser or if the header information was invalid.
	 * @throws SystemException Request header could not be read.
	 */
	bool readAndParseRequestHeaders(FileDescriptor &fd, ScgiRequestParser &parser, string &requestBody) {
		TRACE_POINT();
		char buf[1024 * 16];
		ssize_t size;
		unsigned int accepted = 0;
		
		do {
			size = syscalls::read(fd, buf, sizeof(buf));
			if (size == -1) {
				throw SystemException("Cannot read request header", errno);
			} else if (size == 0) {
				break;
			} else {
				accepted = parser.feed(buf, size);
			}
		} while (parser.acceptingInput());

		if (parser.getState() != ScgiRequestParser::DONE) {
			P_ERROR("Invalid SCGI header received.");
			return false;
		} else if (!parser.hasHeader("DOCUMENT_ROOT")) {
			P_ERROR("DOCUMENT_ROOT header is missing.");
			return false;
		} else {
			requestBody.assign(buf + accepted, size - accepted);
			return true;
		}
	}
	
	/**
	 * Sends a request body to this client. The <tt>partialRequestBody</tt> will first be
	 * sent to the specified <tt>session</tt>, but if the specified <tt>contentLength</tt>
	 * is larger than the size of the <tt>partialRequestBody</tt>, then this method will
	 * attempt to read the remaining bytes from the specified <tt>clientFd</tt> and send it
	 * to the <tt>session</tt> as well until <tt>contentLength</tt> bytes have been sent in
	 * total.
	 *
	 * @param session The Ruby on Rails application instance.
	 * @param clientFd The client file descriptor to send the request body to.
	 * @param partialRequestBody The partial request body to send to this client.
	 * @param contentLength The content length of the request body in bytes.
	 * @throws SystemException Request body could not be read from the specified
	 *   <tt>clientFd</tt>.
	 */
	void sendRequestBody(Application::SessionPtr &session,
	                     FileDescriptor &clientFd,
	                     const string &partialRequestBody,
	                     unsigned long contentLength) {
		TRACE_POINT();
		char buf[1024 * 16];
		ssize_t size;
		size_t bytesToRead;
		unsigned long bytesForwarded = 0;
		
		if (partialRequestBody.size() > 0) {
			UPDATE_TRACE_POINT();
			session->sendBodyBlock(partialRequestBody.c_str(),
				partialRequestBody.size());
			bytesForwarded = partialRequestBody.size();
		}
		
		bool done = bytesForwarded == contentLength;
		while (!done) {
			UPDATE_TRACE_POINT();
			
			bytesToRead = contentLength - bytesForwarded;
			if (bytesToRead > sizeof(buf)) {
				bytesToRead = sizeof(buf);
			}
			size = syscalls::read(clientFd, buf, bytesToRead);
			
			if (size == 0) {
				done = true;
			} else if (size == -1) {
				throw SystemException("Cannot read request body", errno);
			} else {
				UPDATE_TRACE_POINT();
				session->sendBodyBlock(buf, size);
				bytesForwarded += size;
				done = bytesForwarded == contentLength;
			}
		}
	}
	
	/**
	 * Forwards an HTTP response from the given (Rails) <tt>session</tt> to the
	 * given <tt>clientFd</tt>.
	 * 
	 * @param session The Ruby on Rails session to read the response from.
	 * @param clientFd The client file descriptor to write the response to.
	 * @throws SystemException Response could not be read from backend (Rails) process.
	 */
	void forwardResponse(Application::SessionPtr &session, FileDescriptor &clientFd) {
		TRACE_POINT();
		HttpStatusExtractor ex;
		int stream = session->getStream();
		int eof = false;
		MessageChannel output(clientFd);
		char buf[1024 * 32];
		ssize_t size;
		
		/* Read data from the backend process until we're able to
		 * extract the HTTP status line from it.
		 */
		while (!eof) {
			UPDATE_TRACE_POINT();
			size = syscalls::read(stream, buf, sizeof(buf));
			if (size == 0) {
				eof = true;
			} else if (size == -1) {
				throw SystemException("Cannot read response from backend process", errno);
			} else if (ex.feed(buf, size)) {
				/* We now have an HTTP status line. Send back
				 * a proper HTTP response, then exit this while
				 * loop and continue with forwarding the rest
				 * of the response data.
				 */
				UPDATE_TRACE_POINT();
				string statusLine("HTTP/1.1 ");
				statusLine.append(ex.getStatusLine());
				UPDATE_TRACE_POINT();
				output.writeRaw(statusLine.c_str(), statusLine.size());
				UPDATE_TRACE_POINT();
				output.writeRaw(ex.getBuffer().c_str(), ex.getBuffer().size());
				break;
			}
		}
		
		UPDATE_TRACE_POINT();
		while (!eof) {
			UPDATE_TRACE_POINT();
			size = syscalls::read(stream, buf, sizeof(buf));
			if (size == 0) {
				eof = true;
			} else if (size == -1) {
				throw SystemException("Cannot read response from backend process", errno);
			} else {
				UPDATE_TRACE_POINT();
				output.writeRaw(buf, size);
			}
		}
	}
	
	/**
	 * Handles a spawn related exception by writing an appropriate HTTP error response (500)
	 * for the given spawn exception <tt>e</ee> to given file descriptor <tt>fd</tt>'s message
	 * channel.
	 *
	 * @param fd The file descriptor identifying the message channel to write the given
	 *   spawn exception <tt>e</tt> to.
	 * @param e The spawn exception to be written to the given <tt>fd</tt>'s message
	 *   channel.
	 */
	void handleSpawnException(FileDescriptor &fd, const SpawnException &e) {
		MessageChannel channel(fd);
		channel.writeRaw("HTTP/1.1 500 Internal Server Error\x0D\x0A");
		channel.writeRaw("Status: 500 Internal Server Error\x0D\x0A");
		channel.writeRaw("Connection: close\x0D\x0A");
		channel.writeRaw("Content-Type: text/html; charset=utf-8\x0D\x0A");
		
		if (e.hasErrorPage()) {
			channel.writeRaw("Content-Length: " +
				toString(e.getErrorPage().size()) +
				"\x0D\x0A");
			channel.writeRaw("\x0D\x0A");
			channel.writeRaw(e.getErrorPage());
		} else {
			channel.writeRaw("Content-Length: " +
				toString(strlen(e.what())) + "\x0D\x0A");
			channel.writeRaw("\x0D\x0A");
			channel.writeRaw(e.what());
		}
	}
	
	bool handleRequest(FileDescriptor &clientFd) {
		TRACE_POINT();
		ScgiRequestParser parser;
		string partialRequestBody;
		unsigned long contentLength;
		
		if (!readAndCheckPassword(clientFd)) {
			P_ERROR("Client did not send a correct password.");
			return true;
		}
		if (!readAndParseRequestHeaders(clientFd, parser, partialRequestBody)) {
			return true;
		}
		
		try {
			PoolOptions options(canonicalizePath(
				parser.getHeader("DOCUMENT_ROOT") + "/.."));
			options.useGlobalQueue = parser.getHeader("PASSENGER_USE_GLOBAL_QUEUE") == "true";
			options.environment    = parser.getHeader("PASSENGER_ENVIRONMENT");
			options.spawnMethod    = parser.getHeader("PASSENGER_SPAWN_METHOD");
			
			try {
				Application::SessionPtr session(pool->get(options));
			
				UPDATE_TRACE_POINT();
				session->sendHeaders(parser.getHeaderData().c_str(),
					parser.getHeaderData().size());
			
				contentLength = atol(
					parser.getHeader("CONTENT_LENGTH").c_str());
				sendRequestBody(session,
					clientFd,
					partialRequestBody,
					contentLength);
			
				session->shutdownWriter();
				forwardResponse(session, clientFd);
			} catch (const SpawnException &e) {
				handleSpawnException(clientFd, e);
			}
			return false;
		} catch (const boost::thread_interrupted &) {
			throw;
		} catch (const tracable_exception &e) {
			P_ERROR("Uncaught exception in PassengerServer client thread:\n"
				<< "   exception: " << e.what() << "\n"
				<< "   backtrace:\n" << e.backtrace());
			return true;
		} catch (const exception &e) {
			P_ERROR("Uncaught exception in PassengerServer client thread:\n"
				<< "   exception: " << e.what() << "\n"
				<< "   backtrace: not available");
			return true;
		} catch (...) {
			P_ERROR("Uncaught unknown exception in PassengerServer client thread.");
			return true;
		}
	}
	
	void threadMain() {
		TRACE_POINT();
		try {
			while (true) {
				UPDATE_TRACE_POINT();
				FileDescriptor fd(acceptConnection());
				handleRequest(fd);
			}
		} catch (const boost::thread_interrupted &) {
			P_TRACE(2, "Client thread " << this << " interrupted.");
		} catch (const tracable_exception &e) {
			P_ERROR("Uncaught exception in PassengerServer client thread:\n"
				<< "   exception: " << e.what() << "\n"
				<< "   backtrace:\n" << e.backtrace());
			abort();
		} catch (const exception &e) {
			P_ERROR("Uncaught exception in PassengerServer client thread:\n"
				<< "   exception: " << e.what() << "\n"
				<< "   backtrace: not available");
			abort();
		} catch (...) {
			P_ERROR("Uncaught unknown exception in PassengerServer client thread.");
			throw;
		}
	}
	
public:
	Client(unsigned int number, StandardApplicationPoolPtr &pool,
	       const string &password, int serverSocket) {
		this->number = number;
		this->pool = pool;
		this->password = password;
		this->serverSocket = serverSocket;
		thr = new oxt::thread(
			bind(&Client::threadMain, this),
			"Client thread " + toString(number),
			CLIENT_THREAD_STACK_SIZE
		);
	}
	
	~Client() {
		TRACE_POINT();
		this_thread::disable_syscall_interruption dsi;
		this_thread::disable_interruption di;
		
		thr->interrupt_and_join();
		delete thr;
	}
};

typedef shared_ptr<Client> ClientPtr;

class Server {
private:
	static const unsigned int BACKLOG_SIZE = 50;

	string password;
	int adminPipe;
	int serverSocket;
	unsigned int numberOfThreads;
	set<ClientPtr> clients;
	StandardApplicationPoolPtr pool;
	
	void startListening() {
		this_thread::disable_syscall_interruption dsi;
		string socketName = getPassengerTempDir() + "/helper_server.sock";
		struct sockaddr_un addr;
		int ret;
		
		serverSocket = syscalls::socket(PF_UNIX, SOCK_STREAM, 0);
		if (serverSocket == -1) {
			throw SystemException("Cannot create an unconnected Unix socket", errno);
		}
		
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socketName.c_str(), sizeof(addr.sun_path));
		addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
		
		ret = syscalls::bind(serverSocket, (const struct sockaddr *) &addr, sizeof(addr));
		if (ret == -1) {
			int e = errno;
			syscalls::close(serverSocket);
			
			string message("Cannot bind on Unix socket '");
			message.append(socketName);
			message.append("'");
			throw SystemException(message, e);
		}
		
		ret = syscalls::listen(serverSocket, BACKLOG_SIZE);
		if (ret == -1) {
			int e = errno;
			syscalls::close(serverSocket);
			
			string message("Cannot bind on Unix socket '");
			message.append(socketName);
			message.append("'");
			throw SystemException(message, e);
		}
		
		chmod(socketName.c_str(), S_ISVTX |
			S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IWGRP | S_IXGRP |
			S_IROTH | S_IWOTH | S_IXOTH);
	}
	
	void startClientHandlerThreads() {
		for (unsigned int i = 0; i < numberOfThreads; i++) {
			ClientPtr client(new Client(i + 1, pool, password,
				serverSocket));
			clients.insert(client);
		}
	}

public:
	Server(const string &password, const string &rootDir, const string &ruby,
	       int adminPipe, unsigned int maxPoolSize,
	       unsigned int maxInstancesPerApp,
	       unsigned int poolIdleTime) {
		this->password  = password;
		this->adminPipe = adminPipe;
		numberOfThreads = maxPoolSize * 4;
		createPassengerTempDir();
		
		pool = ptr(new StandardApplicationPool(
			rootDir + "/bin/passenger-spawn-server",
			"", ruby
		));
		pool->setMax(maxPoolSize);
		pool->setMaxPerApp(maxInstancesPerApp);
		pool->setMaxIdleTime(poolIdleTime);
		
		startListening();
	}
	
	~Server() {
		TRACE_POINT();
		this_thread::disable_syscall_interruption dsi;
		this_thread::disable_interruption di;
		
		P_DEBUG("Shutting down helper server...");
		clients.clear();
		P_TRACE(2, "All threads have been shut down.");
		syscalls::close(serverSocket);
		syscalls::close(adminPipe);
	}
	
	void start() {
		TRACE_POINT();
		char buf;
		
		startClientHandlerThreads();
		ApplicationPoolStatusReporter reporter(pool);
		
		try {
			syscalls::read(adminPipe, &buf, 1);
		} catch (const boost::thread_interrupted &) {
			// Do nothing.
		}
	}
};

static void
ignoreSigpipe() {
	struct sigaction action;
	action.sa_handler = SIG_IGN;
	action.sa_flags   = 0;
	sigemptyset(&action.sa_mask);
	sigaction(SIGPIPE, &action, NULL);
}

static string
receivePassword(int adminPipe) {
	TRACE_POINT();
	MessageChannel channel(adminPipe);
	char buf[HELPER_SERVER_PASSWORD_SIZE];
	
	if (!channel.readRaw(buf, HELPER_SERVER_PASSWORD_SIZE)) {
		throw IOException("Could not read password from the admin pipe.");
	}
	return string(buf, HELPER_SERVER_PASSWORD_SIZE);
}

int
main(int argc, char *argv[]) {
	TRACE_POINT();
	try {
		setup_syscall_interruption_support();
		ignoreSigpipe();
		
		string password;
		string rootDir  = argv[1];
		string ruby     = argv[2];
		int adminPipe   = atoi(argv[3]);
		int logLevel    = atoi(argv[4]);
		int maxPoolSize = atoi(argv[5]);
		int maxInstancesPerApp = atoi(argv[6]);
		int poolIdleTime       = atoi(argv[7]);
		
		setLogLevel(logLevel);
		P_DEBUG("Passenger helper server started on PID " << getpid());
		
		password = receivePassword(adminPipe);
		P_TRACE(2, "Password received.");
		
		Server(password, rootDir, ruby, adminPipe, maxPoolSize,
			maxInstancesPerApp, poolIdleTime).start();
	} catch (const tracable_exception &e) {
		P_ERROR(e.what() << "\n" << e.backtrace());
		return 1;
	} catch (const exception &e) {
		P_ERROR(e.what());
		return 1;
	} catch (...) {
		P_ERROR("Unknown exception thrown in main thread.");
		throw;
	}
	
	P_TRACE(2, "Helper server exited.");
	return 0;
}
