#include "Server.hpp"
#include "CgiHandler.hpp"
#include "ClientState.hpp"
#include "HttpResponse.hpp"
#include "Logger.hpp"
#include <cstdlib>
#include <sys/_types/_pid_t.h>
#include <sys/event.h>

// -----------------------------------
// Constructor and Destructor
// -----------------------------------

Server::Server(ServerConfig &config, MimeTypeConfig &mimeTypes, KqueueManager &kq)
	: _config(config), _mimeTypes(mimeTypes), _kq(kq), _socket(-1)
{
	_serverAddr.sin_family = AF_INET;
	_serverAddr.sin_port = htons(_config.port);
	_serverAddr.sin_addr.s_addr = inet_addr(_config.ipAddress.c_str());
	memset(_serverAddr.sin_zero, '\0', sizeof(_serverAddr.sin_zero));
}

Server::~Server()
{
	std::map<int, ClientState *>::iterator client = _clients.begin();
	while (client != _clients.end())
	{
		_kq.unregisterEvent(client->first, EVFILT_READ);
		delete client->second;
		close(client->first);
		client++;
	}
	_clients.clear();

	std::map<int, ResponseState *>::iterator response = _responses.begin();
	while (response != _responses.end())
	{
		_kq.unregisterEvent(response->first, EVFILT_WRITE);
		delete response->second;
		response++;
	}
	_responses.clear();

	if (_socket != -1)
	{
		_kq.unregisterEvent(_socket, EVFILT_READ);
		close(_socket);
	}
}

// -----------------------------------
// Server Creation
// -----------------------------------

void	Server::createServerSocket()
{
	this->_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (this->_socket < 0)
	{
		Logger::log(Logger::ERROR, "Failed to create server socket: " + std::string(strerror(errno)), "Server::createServerSocket");
		this->_socket = -1;
	}
	else
		Logger::log(Logger::INFO, "Server socket created successfully", "Server::createServerSocket");
}

void	Server::setSocketOptions()
{
	if (_socket == -1)
		return;

	int opt = 1;
	if (setsockopt(this->_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
	{
		Logger::log(Logger::ERROR, "Failed to set socket options: " + std::string(strerror(errno)), "Server::setSocketOptions");
		_socket = -1;
	}
	else
		Logger::log(Logger::INFO, "Socket options set successfully", "Server::setSocketOptions");
}


void	Server::setSocketToNonBlocking()
{
	if (_socket == -1)
		return;

	int flags = fcntl(this->_socket, F_GETFL, 0);
	if (flags < 0)
	{
		Logger::log(Logger::ERROR, "fcntl(F_GETFL) failed: " + std::string(strerror(errno)), "Server::setSocketToNonBlocking");
        _socket = -1;
		return;
	}
	if (fcntl(this->_socket, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		Logger::log(Logger::ERROR, "fcntl(F_SETFL) failed: " + std::string(strerror(errno)), "Server::setSocketToNonBlocking");
		_socket = -1;
	}
	else
		Logger::log(Logger::INFO, "Socket set to non-blocking mode successfully", "Server::setSocketToNonBlocking");
}

void	Server::bindAndListen()
{
	if (_socket == -1)
		return;

	if (bind(this->_socket, (const struct sockaddr *)(&this->_serverAddr), sizeof(this->_serverAddr)) < 0)
	{
		Logger::log(Logger::ERROR, "Failed to bind socket: " + std::string(strerror(errno)), "Server::bindAndListen");
		_socket = -1;
		return;
	}
	Logger::log(Logger::INFO, "Socket bound successfully", "Server::bindAndListen");
	if (listen(this->_socket, SERVER_BACKLOG) < 0)
	{
		Logger::log(Logger::ERROR, "Failed to listen on socket: " + std::string(strerror(errno)), "Server::bindAndListen");
		_socket = -1;
	}
	else
		Logger::log(Logger::INFO, "Server is now listening on socket", "Server::bindAndListen");
}

void	Server::run()
{
	createServerSocket();
	setSocketOptions();
	setSocketToNonBlocking();
	bindAndListen();
}

// -----------------------------------
// Client Connection Handling
// -----------------------------------

void	Server::acceptNewConnection()
{
	struct sockaddr_in	clientAddr;
	socklen_t			clientAddrLen = sizeof(clientAddr);
	int clientSocket = accept(this->_socket, (struct sockaddr *)&clientAddr, &clientAddrLen);
	if (clientSocket < 0)
	{
		Logger::log(Logger::ERROR, "Error accepting new connection: " + std::string(strerror(errno)), "Server::acceptNewConnection");
		return;
	}
	Logger::log(Logger::INFO, "Accepted new connection on socket fd " + std::to_string(clientSocket), "Server::acceptNewConnection");
	ClientState *clientState = new ClientState(clientSocket, inet_ntoa(clientAddr.sin_addr));
	_clients[clientSocket] = clientState;
	_kq.registerEvent(clientSocket, EVFILT_READ);
}

void	Server::handleClientDisconnection(int clientSocket)
{
	Logger::log(Logger::INFO, "Handling disconnection of client with socket fd " + std::to_string(clientSocket), "Server::handleClientDisconnection");

	_kq.unregisterEvent(clientSocket, EVFILT_READ);
	ClientState *clientState = _clients[clientSocket];
	_clients.erase(clientSocket);
	delete clientState;
	if (_responses.count(clientSocket) > 0)
	{
		_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
		ResponseState *responseState = _responses[clientSocket];
		_responses.erase(clientSocket);
		delete responseState;
	}
	close(clientSocket);
}

// -----------------------------------
// Request Processing
// -----------------------------------


void	Server::handleClientRequest(int clientSocket)
{
	char buffer[BUFFER_SIZE];
	size_t bytesRead = recv(clientSocket, buffer, BUFFER_SIZE, 0);
	if (bytesRead < 0)
	{
		Logger::log(Logger::ERROR, "Error receiving data from client with socket fd " + std::to_string(clientSocket), "Server::handleClientRequest");
		removeClient(clientSocket);
		close(clientSocket);
	}

	if (bytesRead > 0)
	{
		ClientState *client = _clients[clientSocket];

		client->updateLastRequestTime();
		client->incrementRequestCount();

		Logger::log(Logger::DEBUG, "Received new request from client with socket fd " + std::to_string(clientSocket), "Server::handleClientRequest");
		client->processIncomingData(*this, buffer, bytesRead);
	}
}

bool			Server::validateFileExtension(HttpRequest &request)
{
	std::vector<std::string>	cgiExten = _config.cgiExtension.getExtensions();
	std::string					uri = request.getUri();

	// std::cout << "uri cgi = " << uri.substr(uri.find('.'), uri.length()) << std::endl;
	// std::vector<std::string>::iterator it = cgiExten.begin();
	// for (; it != cgiExten.end(); it++)
	// 	std::cout << "{" << *it << "}" << std::endl;
	if (uri.find('.') == std::string::npos ||
	std::find(cgiExten.begin(), cgiExten.end(),
	uri.substr(uri.find('.'), uri.length())) == cgiExten.end())
		return false;
	return true;
}

bool	Server::fileExists(const std::string &path)
{
	struct stat fileStat;

	if (stat(path.c_str(), &fileStat) == 0)
		return (true);
	return (false);
}

bool			Server::validCgiRequest(HttpRequest &request, ServerConfig &config)
{
	if (((config.root).find("/cgi-bin") == std::string::npos && (config.root + request.getUri()).find("/cgi-bin") == std::string::npos)
	|| !this->fileExists(config.root + request.getUri()) || !validateFileExtension(request))
		return false;
	return true;
}

void	Server::handleCgiOutput(int pipeReadFd)
{
	Logger::log(Logger::INFO, "Handling CGI output", "Server::handleCgiOutput");

	char	buffer[BUFFER_SIZE];
	ssize_t	bytesRead = read(pipeReadFd, buffer, BUFFER_SIZE);
	if (bytesRead < 0)
	{
		Logger::log(Logger::ERROR, "Error reading from CGI pipe: " + std::string(strerror(errno)), "Server::handleCgiOutput");
		return ;
	}
	else if (bytesRead == 0)
	{
		Logger::log(Logger::INFO, "Finished reading from CGI pipe", "Server::handleCgiOutput");
		CgiState *cgiState = _cgiStates[pipeReadFd];
		HttpResponse response;
		response.setVersion("HTTP/1.1");
		response.setStatusCode(std::to_string(200));
		response.setStatusMessage("OK");
		response.setBody(cgiState->_cgiResponseMessage);
		response.setHeader("Content-Length", std::to_string(response.getBody().length()));
		response.setHeader("Content-Type", "text/plain");
		response.setHeader("Server", "Nginx 2.0");
		response.setHeader("Connection", "keep-alive");
		ResponseState *responseState = new ResponseState(response.buildResponse());
		_clients[cgiState->_clientSocket]->resetClientState();
		_responses[cgiState->_clientSocket] = responseState;
		_kq.registerEvent(cgiState->_clientSocket, EVFILT_WRITE);
		_kq.unregisterEvent(pipeReadFd, EVFILT_READ);
		_cgiStates.erase(pipeReadFd);
		close(pipeReadFd);
		delete cgiState;
	}
	else {
		CgiState *cgiState = _cgiStates[pipeReadFd];
		cgiState->_cgiResponseMessage += std::string(buffer, bytesRead);
		if (cgiState->_cgiResponseMessage.length() > CGI_MAX_OUTPUT_SIZE)
		{
			Logger::log(Logger::WARN, "CGI response size exceeded the maximum limit", "Server::handleCgiOutput");

			_kq.unregisterEvent(pipeReadFd, EVFILT_READ);
			handleInvalidRequest(cgiState->_clientSocket, 500, "The CGI script's output exceeded the maximum allowed size of 2 MB and was terminated.");
			kill(cgiState->_pid, SIGKILL);
			_cgiStates.erase(pipeReadFd);
			close(pipeReadFd);
			delete cgiState;
		}
	}
}

void	Server::handleCgiRequest(int clientSocket, HttpRequest &request)
{
	pid_t	pid;
	int		pipeFd[2];
	char	**params;

	params = new char *[2];
	params[0] = strdup((_config.root + request.getUri()).c_str());
	params[1] = NULL;

	if (pipe(pipeFd) < 0)
	{
		Logger::log(Logger::ERROR, "Pipe Error", "Server::handleCgiRequest");
		return ;
	}

	pid = fork();
	if (pid < 0)
	{
		Logger::log(Logger::ERROR, "Fork Error", "Server::handleCgiRequest");
		return ;
	}
	else if (pid == 0)
	{
		close(pipeFd[0]);
		dup2(pipeFd[1], STDOUT_FILENO);
		close(pipeFd[1]);

		if (execve(params[0], params, NULL) < 0)
		{
			Logger::log(Logger::ERROR, "Execve Error", "Server::handleCgiRequest");
			delete params[0];
			delete [] params;
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	}
	else
	{
		close(pipeFd[1]);
		if (fcntl(pipeFd[0], F_SETFL, O_NONBLOCK) < 0)
		{
			Logger::log(Logger::ERROR, "Fcntl Error", "Server::handleCgiRequest");
			return ;
		}
		Logger::log(Logger::DEBUG, "Registering CGI read end of the pipe fd " + std::to_string(pipeFd[0]) + " for read events", "Server::handleCgiRequest");
		_kq.registerEvent(pipeFd[0], EVFILT_READ);
		CgiState *cgiState = new CgiState(pid, pipeFd[0], clientSocket);
		_cgiStates[pipeFd[0]] = cgiState;

		delete params[0];
		delete [] params;
	}
	

}

void	Server::processGetRequest(int clientSocket, HttpRequest &request)
{
	if (_config.cgiExtension.isEnabled() && validCgiRequest(request, _config))
	{
			// Logger::log(Logger::ERROR, "Here", "1");
			// std::cout << "Valid Cgi" << std::endl;
			// CgiHandler	*cgiDirective = new CgiHandler(request, _config, _kq);
			// _cgi[clientSocket] = cgiDirective;
		Logger::log(Logger::INFO, "Handling 'CGI GET' request", "Server::processGetRequest");
		handleCgiRequest(clientSocket, request);
	}
	else
	{
		ResponseState *responseState;
		RequestHandler handler(_config, _mimeTypes);
		HttpResponse response = handler.handleRequest(request);
		_clients[clientSocket]->resetClientState();
		
		if (response.getType() == SMALL_RESPONSE)
			responseState = new ResponseState(response.buildResponse());
		else
			responseState = new ResponseState(response.buildResponse(), response.getFilePath(), response.getFileSize());

		_responses[clientSocket] = responseState;
		_kq.registerEvent(clientSocket, EVFILT_WRITE);
	}
}

void	Server::processPostRequest(int clientSocket, HttpRequest &request, bool closeConnection)
{

	if (validCgiRequest(request, _config))
	{
		std::cout << "Valid Cgi" << std::endl;
	}
	else
	{
		ResponseState *responseState;
		RequestHandler handler(_config, _mimeTypes);
		HttpResponse response = handler.handleRequest(request);
		_clients[clientSocket]->resetClientState();
		
		responseState = new ResponseState(response.buildResponse(), closeConnection);

		_responses[clientSocket] = responseState;
		_kq.registerEvent(clientSocket, EVFILT_WRITE);
	}
}

// -----------------------------------
// Response Handling
// -----------------------------------


void	Server::handleClientResponse(int clientSocket)
{
	if (_responses.count(clientSocket) == 0)
	{
		Logger::log(Logger::ERROR, "No response state found for client socket " + std::to_string(clientSocket), "Server::handleClientResponse");
		_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
		return;
	}
	ResponseState *responseState = _responses[clientSocket];

	if (responseState->getType() == SMALL_RESPONSE)
		sendSmallResponse(clientSocket, responseState);
	else if (responseState->getType() == LARGE_RESPONSE)
		sendLargeResponse(clientSocket, responseState);
}

void	Server::sendSmallResponse(int clientSocket, ResponseState *responseState)
{
	Logger::log(Logger::DEBUG, "Sending small response to client with socket fd " + std::to_string(clientSocket), "Server::sendSmallResponse");

	const std::string &response = responseState->getSmallResponse();
	size_t totalLength = response.length();
	size_t remainingLength = totalLength - responseState->bytesSent;
	const char *responsePtr = response.c_str() + responseState->bytesSent;

	ssize_t bytesSent = send(clientSocket, responsePtr, remainingLength, 0);

	if (bytesSent < 0)
	{
		Logger::log(Logger::ERROR, "Failed to send small response to client with socket fd " + std::to_string(clientSocket) + ". Error: " + strerror(errno), "Server::sendSmallResponse");
		_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
		_responses.erase(clientSocket);
		delete responseState;
	}
	else
	{
		responseState->bytesSent += bytesSent;
		if (responseState->isFinished())
		{
			Logger::log(Logger::DEBUG, "Small response sent completely to client with socket fd " + std::to_string(clientSocket), "Server::sendSmallResponse");
			_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
			_responses.erase(clientSocket);
			if (responseState->closeConnection)
			{
				Logger::log(Logger::INFO, "Closing connection after sending small response to client with socket fd " + std::to_string(clientSocket), "Server::sendSmallResponse");
				close(clientSocket);
			}
			delete responseState;
		}
		else
			Logger::log(Logger::DEBUG, "Partial small response sent to client with socket fd " + std::to_string(clientSocket), "Server::sendSmallResponse");
	}
}

void	Server::sendLargeResponse(int clientSocket, ResponseState *responseState)
{
	if (!responseState->isHeaderSent)
		sendLargeResponseHeaders(clientSocket, responseState);
	else
		sendLargeResponseChunk(clientSocket, responseState);
}

void	Server::sendLargeResponseHeaders(int clientSocket, ResponseState *responseState)
{
	Logger::log(Logger::DEBUG, "Sending large response headers to client with socket fd " + std::to_string(clientSocket), "Server::sendLargeResponseHeaders");

	const std::string &headers = responseState->getHeaders();
	size_t totalLength = headers.length();
	size_t remainingLength = totalLength - responseState->headersSent;
	const char *headersPtr = headers.c_str() + responseState->headersSent;

	ssize_t bytesSent = send(clientSocket, headersPtr, remainingLength, 0);
	if (bytesSent < 0)
	{
		Logger::log(Logger::ERROR, "Failed to send large response headers to client with socket fd " + std::to_string(clientSocket) + ". Error: " + strerror(errno), "Server::sendLargeResponseHeaders");
		_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
		_responses.erase(clientSocket);
		delete responseState;
	}
	else
	{
		responseState->headersSent += bytesSent;
		if (responseState->headersSent >= totalLength)
		{
			Logger::log(Logger::DEBUG, "Large response headers sent completely to client with socket fd " + std::to_string(clientSocket), "Server::sendLargeResponseHeaders");
			responseState->isHeaderSent = true;
		}
		else
			Logger::log(Logger::DEBUG, "Partial large response headers sent to client with socket fd " + std::to_string(clientSocket), "Server::sendLargeResponseHeaders");
	}
}


void	Server::sendLargeResponseChunk(int clientSocket, ResponseState *responseState)
{
	Logger::log(Logger::DEBUG, "Sending Large Response chunk to client with socket fd " + std::to_string(clientSocket), "Server::sendLargeResponseChunk");

	std::string chunk = responseState->getNextChunk();
	size_t totalLength = chunk.length();
	size_t remainingLength = totalLength - responseState->currentChunkPosition;
	const char *chunkPtr = chunk.c_str() + responseState->currentChunkPosition;
	ssize_t bytesSent = send(clientSocket, chunkPtr, remainingLength, 0);

	if (bytesSent < 0)
	{
		Logger::log(Logger::ERROR, "Failed to send Large Response chunk to client with socket fd " + std::to_string(clientSocket) + ". Error: " + strerror(errno), "Server::sendLargeResponseChunk");
		_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
		_responses.erase(clientSocket);
		delete responseState;
	}
	else
	{
		responseState->currentChunkPosition += bytesSent;
		if (responseState->currentChunkPosition >= totalLength)
		{
			Logger::log(Logger::DEBUG, "Chunk sent completely to client with socket fd " + std::to_string(clientSocket) + " and this what has been sent : " + std::to_string(bytesSent), "Server::sendLargeResponseChunk");
			responseState->currentChunkPosition = 0;
			if (responseState->isFinished())
			{
				// send end chunk
				std::string endChunk = "0\r\n\r\n";
				bytesSent = send(clientSocket, endChunk.c_str(), endChunk.length(), 0);
				if (bytesSent < 0)
				{
					Logger::log(Logger::ERROR, "Failed to send end chunk to client with socket fd " + std::to_string(clientSocket) + ". Error: " + strerror(errno), "Server::sendLargeResponseChunk");
					_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
					_responses.erase(clientSocket);
					delete responseState;
				}
				else
				{
					Logger::log(Logger::DEBUG, "End chunk sent completely to client with socket fd " + std::to_string(clientSocket), "Server::sendLargeResponseChunk");
					_kq.unregisterEvent(clientSocket, EVFILT_WRITE);
					_responses.erase(clientSocket);
					delete responseState;
				}
			}
		}
		else
		{
			Logger::log(Logger::DEBUG, "Partial chunk sent to client with socket fd " + std::to_string(clientSocket) + " and this what has been sent: " + std::to_string(bytesSent), "Server::sendLargeResponseChunk");
		}
	}

}

// -----------------------------------
// Error Handling
// -----------------------------------


void	Server::handleHeaderSizeExceeded(int clientSocket)
{
	Logger::log(Logger::WARN, "Request headers size exceeded the maximum limit for fd " + std::to_string(clientSocket), "Server::handleHeaderSizeExceeded");

	HttpResponse response;

	removeClient(clientSocket);
	response.generateStandardErrorResponse("400", "Bad Request", "400 Request Header Or Cookie Too Large", "Request Header Or Cookie Too Large");
	ResponseState *responseState = new ResponseState(response.buildResponse(), true);
	_responses[clientSocket] = responseState;
	_kq.registerEvent(clientSocket, EVFILT_WRITE);
}

void	Server::handleUriTooLarge(int clientSocket)
{
	Logger::log(Logger::WARN, "URI size exceeded the maximum limit for fd " + std::to_string(clientSocket), "Server::handleUriTooLarge");
	
	HttpResponse response;

	removeClient(clientSocket);
	response.generateStandardErrorResponse("414", "Request-URI Too Large", "414 Request-URI Too Large");
	ResponseState *responseState = new ResponseState(response.buildResponse(), true);
	_responses[clientSocket] = responseState;
	_kq.registerEvent(clientSocket, EVFILT_WRITE);
}

void	Server::handleInvalidGetRequest(int clientSocket)
{
	Logger::log(Logger::WARN, "GET request with body received for fd " + std::to_string(clientSocket), "Server::handleInvalidGetRequest");
	
	HttpResponse response;

	removeClient(clientSocket);
	response.generateStandardErrorResponse("400", "Bad Request", "400 Invalid GET Request (with body indicators)", "Invalid GET Request (with body indicators)");
	ResponseState *responseState = new ResponseState(response.buildResponse(), true);
	_responses[clientSocket] = responseState;
	_kq.registerEvent(clientSocket, EVFILT_WRITE);
}


void	Server::handleInvalidRequest(int clientSocket, int requestStatusCode, const std::string &detail)
{
	std::string statusCode = std::to_string(requestStatusCode);
	std::string statusMessage = getStatusMessage(requestStatusCode);

	HttpResponse response;
	removeClient(clientSocket);
	response.generateStandardErrorResponse(statusCode, statusMessage, statusCode + " " + statusMessage, detail);
	ResponseState *responseState = new ResponseState(response.buildResponse(), true);
	_responses[clientSocket] = responseState;
	_kq.registerEvent(clientSocket, EVFILT_WRITE);
}

// -----------------------------------
// Timeout and Cleanup
// -----------------------------------


void	Server::checkForTimeouts()
{
	std::map<int, ClientState *>::iterator it = _clients.begin();
	while (it != _clients.end())
	{
		if (it->second->isTimedOut(this->_config.keepalive_timeout))
		{
			Logger::log(Logger::INFO, "Client with socket fd " + std::to_string(it->first) + " timed out and is being disconnected", "Server::checkForTimeouts");
			_kq.unregisterEvent(it->first, EVFILT_READ);
			close(it->first);
			delete it->second;
			it = _clients.erase(it);
		}
		else
			it++;
	}
}

void	Server::checkForCgiTimeouts()
{
	std::map<int, CgiState *>::iterator it = _cgiStates.begin();
	while (it != _cgiStates.end())
	{
		if (it->second->isTimedOut(CGI_TIMEOUT))
		{
			Logger::log(Logger::INFO, "Cgi with socket fd " + std::to_string(it->first) + " timed out and is being disconnected", "Server::checkForCgiTimeouts");

			_kq.unregisterEvent(it->first, EVFILT_READ);
			close(it->first);
			kill(it->second->_pid, SIGKILL);
			handleInvalidRequest(it->second->_clientSocket, 504, "The CGI script failed to complete in a timely manner. Please try again later.");
			delete it->second;
			it = _cgiStates.erase(it);
			
		}
		else
			it++;
	}
}

void	Server::removeClient(int clientSocket)
{
	if (_clients.find(clientSocket) == _clients.end())
	{
        Logger::log(Logger::WARN, "Attempted to remove non-existent client with socket fd " + std::to_string(clientSocket), "Server::removeClient");
        return;
    }

	Logger::log(Logger::INFO, "Removing client with socket fd " + std::to_string(clientSocket), "Server::removeClient");

	ClientState *clientState = _clients[clientSocket];
	_kq.unregisterEvent(clientSocket, EVFILT_READ);
	_clients.erase(clientSocket);
	delete clientState;
}

std::string	Server::getStatusMessage(int statusCode)
{
	std::map<int, std::string>			statusCodeMessages;

	statusCodeMessages[400] = "Bad Request";
	statusCodeMessages[411] = "Length Required";
	statusCodeMessages[413] = "Request Entity Too Large";
	statusCodeMessages[414] = "Request-URI Too Large";
	statusCodeMessages[500] = "Internal Server Error";
	statusCodeMessages[501] = "Not Implemented";
	statusCodeMessages[503] = "Service Unavailable";
	statusCodeMessages[504] = "Gateway Timeout";
	statusCodeMessages[505] = "HTTP Version Not Supported";

	if (statusCodeMessages.count(statusCode) == 0)
		return "";
	return statusCodeMessages[statusCode];
}


// ----------------------------------- CGI_STATE -----------------------------------
CgiState::CgiState(pid_t pid, int readFd, int clientSocket)
	: _pid(pid), _pipeReadFd(readFd), _clientSocket(clientSocket)
{
	this->_startTime = std::chrono::steady_clock::now();
}

bool	CgiState::isTimedOut(size_t timeout) const
{
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (std::chrono::duration_cast<std::chrono::seconds>(now - _startTime) > std::chrono::seconds(timeout))
		return true;
	return false;
}
