

#pragma once
#ifndef HTTPRESPONSE_HPP
#define HTTPRESPONSE_HPP

#include <fstream>
#include <string>
#include <map>

#define CHUNK_SIZE 8192 // 8 KB


class HttpResponse
{

	private:

		std::string							version;
		std::string							statusCode;
		std::string							statusMessage;
		std::map<std::string, std::string>	headers;
		std::string							body;

	public:

		HttpResponse();


		void	setVersion(const std::string& version);
		void	setStatusCode(const std::string& statusCode);
		void	setStatusMessage(const std::string& statusMessage);
		void	setHeader(const std::string& key, const std::string& value);
		void	setBody(const std::string& body);


		std::string	getVersion() const;
		std::string	getStatusCode() const;
		std::string	getStatusMessage() const;
		std::string	getHeader(const std::string& key) const;
		std::string	getBody() const;


		std::string getStatusLine() const;
		std::string getHeadersAsString() const;

		std::string buildResponse() const;



};

class ResponseState
{

public:
	enum	ResponseType { SMALL_FILE, LARGE_FILE };

	ResponseState(const std::string &smallFileResponse); // small file
	ResponseState(const std::string &responseHeaders, const std::string &filePath, size_t fileSize); // large file

	ResponseType		getType() const;

	const std::string	&getSmallFileResponse() const;

	std::string			getNextChunk();

	bool				isFinished() const;

private:
	ResponseType	type;
	std::string		smallFileResponse;
	std::string		headers;
	std::string		filePath;
	std::ifstream	fileStream;
	size_t			fileSize;
	size_t			bytesSent;

};
















#endif /* HTTPRESPONSE_HPP */