#include "sharepoint_requests.hpp"

#include "duckdb.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/config.hpp"

#include <cassert>
#include <deque>
#include <string>
#include <vector>

using namespace duckdb;

struct RecordedRequest {
	RequestType type;
	std::string url;
	HTTPHeaders headers;
	std::string body;
};

struct MockResponse {
	HTTPStatusCode status;
	std::string body;
	std::string location;
};

class MockHTTPUtil : public HTTPUtil {
public:
	std::deque<MockResponse> responses;
	std::vector<RecordedRequest> requests;

	unique_ptr<HTTPResponse> SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) override {
		(void)client;
		assert(!responses.empty());

		std::string body;
		if (request.type == RequestType::POST_REQUEST) {
			auto &post = request.Cast<PostRequestInfo>();
			body.assign(const_char_ptr_cast(post.buffer_in), post.buffer_in_len);
		} else if (request.type == RequestType::PUT_REQUEST) {
			auto &put = request.Cast<PutRequestInfo>();
			body.assign(const_char_ptr_cast(put.buffer_in), put.buffer_in_len);
		}
		requests.push_back({request.type, request.url, request.headers, body});

		auto mock = std::move(responses.front());
		responses.pop_front();
		auto response = make_uniq<HTTPResponse>(mock.status);
		response->body = std::move(mock.body);
		if (request.type == RequestType::POST_REQUEST) {
			request.Cast<PostRequestInfo>().buffer_out = response->body;
		}
		if (!mock.location.empty()) {
			response->headers.Insert("Location", std::move(mock.location));
		}
		const auto status_code = static_cast<uint16_t>(response->status);
		response->success = status_code >= 200 && status_code < 300;
		return response;
	}
};

struct TestContext {
	DuckDB database;
	Connection connection;
	shared_ptr<MockHTTPUtil> http;

	TestContext() : database(nullptr), connection(database), http(make_shared_ptr<MockHTTPUtil>()) {
		DBConfig::GetConfig(*connection.context).SetHTTPUtil(http);
	}
};

static void TestGetHeadersAndJsonBody() {
	TestContext test;
	test.http->responses.push_back({HTTPStatusCode::OK_200, R"({"ok":true})", ""});

	const auto body = PerformHttpsRequest(*test.connection.context, "graph.microsoft.com", "/v1.0/me", "secret-token");

	assert(body == R"({"ok":true})");
	assert(test.http->requests.size() == 1);
	const auto &request = test.http->requests[0];
	assert(request.type == RequestType::GET_REQUEST);
	assert(request.url == "https://graph.microsoft.com/v1.0/me");
	assert(request.headers.GetHeaderValue("Authorization") == "Bearer secret-token");
	assert(request.headers.GetHeaderValue("Accept") == "application/json");
}

static void TestPostBodyAndContentType() {
	TestContext test;
	test.http->responses.push_back({HTTPStatusCode::OK_200, R"({"access_token":"token"})", ""});
	const std::string request_body = "grant_type=refresh_token&refresh_token=abc";

	const auto response =
	    PerformHttpsRequest(*test.connection.context, "login.microsoftonline.com", "/common/oauth2/v2.0/token", "",
	                        HttpMethod::POST, request_body, "application/x-www-form-urlencoded");

	assert(response == R"({"access_token":"token"})");
	assert(test.http->requests.size() == 1);
	const auto &request = test.http->requests[0];
	assert(request.type == RequestType::POST_REQUEST);
	assert(request.body == request_body);
	assert(request.headers.GetHeaderValue("Content-Type") == "application/x-www-form-urlencoded");
	assert(!request.headers.HasHeader("Authorization"));
}

static void TestBinaryResponse() {
	TestContext test;
	const std::string binary_body("\x50\x4b\x03\x04\x00\xff", 6);
	test.http->responses.push_back({HTTPStatusCode::OK_200, binary_body, ""});

	const auto response = PerformHttpsRequest(*test.connection.context, "download.example.com", "/file.xlsx", "",
	                                          HttpMethod::GET, "", "application/json", "*/*");
	assert(response == binary_body);
}

static void TestHttpErrorIncludesBody() {
	TestContext test;
	test.http->responses.push_back({HTTPStatusCode::BadRequest_400, R"({"error":"bad request"})", ""});

	try {
		PerformHttpsRequest(*test.connection.context, "graph.microsoft.com", "/broken", "");
		assert(false);
	} catch (const IOException &ex) {
		const std::string message = ex.what();
		assert(message.find("HTTP 400") != std::string::npos);
		assert(message.find("bad request") != std::string::npos);
	}
}

static void TestSameHostRedirectKeepsToken() {
	TestContext test;
	test.http->responses.push_back(
	    {HTTPStatusCode::TemporaryRedirect_307, "", "https://graph.microsoft.com/v1.0/redirected"});
	test.http->responses.push_back({HTTPStatusCode::OK_200, "done", ""});

	const auto response =
	    PerformHttpsRequest(*test.connection.context, "graph.microsoft.com", "/v1.0/start", "secret-token");

	assert(response == "done");
	assert(test.http->requests.size() == 2);
	assert(test.http->requests[1].headers.GetHeaderValue("Authorization") == "Bearer secret-token");
}

static void TestCrossHostRedirectStripsToken() {
	TestContext test;
	test.http->responses.push_back(
	    {HTTPStatusCode::TemporaryRedirect_307, "", "https://download.example.com/signed/file.xlsx"});
	test.http->responses.push_back({HTTPStatusCode::OK_200, "file-content", ""});

	const auto response =
	    PerformHttpsRequest(*test.connection.context, "graph.microsoft.com", "/v1.0/content", "secret-token");

	assert(response == "file-content");
	assert(test.http->requests.size() == 2);
	assert(!test.http->requests[1].headers.HasHeader("Authorization"));
}

int main() {
	TestGetHeadersAndJsonBody();
	TestPostBodyAndContentType();
	TestBinaryResponse();
	TestHttpErrorIncludesBody();
	TestSameHostRedirectKeepsToken();
	TestCrossHostRedirectStripsToken();
	return 0;
}
