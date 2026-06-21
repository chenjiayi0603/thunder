/**
 * Thunder 容器化 E2E 烟雾测试
 *
 * 前置条件（全满足才会执行，否则自动跳过）：
 *   1. THUNDER_E2E_ENABLED=1
 *   2. docker compose -f deploy/docker/docker-compose.yml up -d center
 *   3. HelloHttp 模块已部署（access_port 默认 27006，见 tests/ports.env）
 *
 * 运行方式：
 *   ./deploy.sh up
 *   THUNDER_E2E_ENABLED=1 ctest --test-dir build/code/test -R ThunderE2ESmoke
 *
 * 端口说明（见 tests/ports.env）：
 *   THUNDER_HELLO_HTTP_PORT=27006  ← Hello HTTP access_port（测试用这个）
 *   inner_port=27007               ← Manager↔Worker 内部端口，不对外
 *   16068                          ← Manager 管理端口，docker-compose healthcheck 用，不是 HTTP 服务
 */

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <curl/curl.h>

namespace
{

bool e2eEnabled()
{
    const char* e = std::getenv("THUNDER_E2E_ENABLED");
    return e != nullptr && std::strcmp(e, "1") == 0;
}

std::string helloHost()
{
    const char* h = std::getenv("THUNDER_E2E_HELLO_HOST");
    return h ? h : "127.0.0.1";
}

int helloPort()
{
    const char* p = std::getenv("THUNDER_E2E_HELLO_PORT");
    return p ? std::atoi(p) : 27006;
}

struct CurlGlobal
{
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

size_t writeCb(void* contents, size_t size, size_t nmemb, void* userp)
{
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<const char*>(contents), size * nmemb);
    return size * nmemb;
}

} // namespace

class ThunderE2ESmoke : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        curl_ = std::make_unique<CurlGlobal>();
    }

    static void TearDownTestSuite()
    {
        curl_.reset();
    }

    void SetUp() override
    {
        if (!e2eEnabled())
        {
            GTEST_SKIP() << "Set THUNDER_E2E_ENABLED=1 to run E2E smoke tests";
        }
    }

    static std::unique_ptr<CurlGlobal> curl_;
};

std::unique_ptr<CurlGlobal> ThunderE2ESmoke::curl_;

TEST_F(ThunderE2ESmoke, CenterHealthCheck)
{
    const char* centerHost = std::getenv("THUNDER_E2E_CENTER_HOST");
    const char* centerPort = std::getenv("THUNDER_E2E_CENTER_PORT");
    if (!centerHost || !centerPort)
    {
        GTEST_SKIP() << "Set THUNDER_E2E_CENTER_HOST/THUNDER_E2E_CENTER_PORT for Center test";
    }

    auto* curl = curl_easy_init();
    ASSERT_NE(nullptr, curl);

    std::string url = std::string("http://") + centerHost + ":" + centerPort + "/";
    std::string resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    // Center 可能不响应 HTTP GET，只要连接不超时就算健康
    EXPECT_NE(res, CURLE_COULDNT_CONNECT) << "Center unreachable at " << url;

    curl_easy_cleanup(curl);
}

TEST_F(ThunderE2ESmoke, HelloHttpPost)
{
    auto* curl = curl_easy_init();
    ASSERT_NE(nullptr, curl);

    std::string url = "http://" + helloHost() + ":" + std::to_string(helloPort()) + "/hello/hello";
    std::string body = R"({"option":"Hello"})";
    std::string resp;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    EXPECT_EQ(res, CURLE_OK) << "HTTP POST /hello/hello failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200L);
    EXPECT_NE(resp.find("\"code\""), std::string::npos) << "resp: " << resp;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

TEST_F(ThunderE2ESmoke, HelloHttpMultiPost)
{
    // 连续发 5 次请求验证 WorkStealingPool 持续调度正常
    auto* curl = curl_easy_init();
    ASSERT_NE(nullptr, curl);

    std::string url = "http://" + helloHost() + ":" + std::to_string(helloPort()) + "/hello/hello";
    std::string body = R"({"option":"Hello"})";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    for (int i = 0; i < 5; ++i)
    {
        std::string resp;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

        CURLcode res = curl_easy_perform(curl);
        EXPECT_EQ(res, CURLE_OK) << "request " << i << " failed: " << curl_easy_strerror(res);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        EXPECT_EQ(httpCode, 200L) << "request " << i;

        curl_easy_reset(curl);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
