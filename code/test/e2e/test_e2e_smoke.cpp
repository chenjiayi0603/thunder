/**
 * Thunder 容器化 E2E 烟雾测试
 *
 * 前置条件（全满足才会执行，否则自动跳过）：
 *   1. THUNDER_E2E_ENABLED=1
 *   2. docker compose -f deploy/docker/docker-compose.yml up -d center
 *   3. HelloHttp 模块已部署在 127.0.0.1:16068（默认端口）
 *
 * 运行方式：
 *   cd deploy/docker && docker compose up -d center hello
 *   THUNDER_E2E_ENABLED=1 THUNDER_E2E_HELLO_HOST=127.0.0.1 \
 *     THUNDER_E2E_HELLO_PORT=16068 ctest --test-dir code/test -R ThunderE2E
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
    return p ? std::atoi(p) : 16068;
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

TEST_F(ThunderE2ESmoke, HelloHttpGet)
{
    auto* curl = curl_easy_init();
    ASSERT_NE(nullptr, curl);

    std::string url = "http://" + helloHost() + ":" + std::to_string(helloPort()) + "/";
    std::string resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    EXPECT_EQ(res, CURLE_OK) << "HTTP GET failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200L);

    curl_easy_cleanup(curl);
}

TEST_F(ThunderE2ESmoke, HelloHttpPostEcho)
{
    auto* curl = curl_easy_init();
    ASSERT_NE(nullptr, curl);

    std::string url = "http://" + helloHost() + ":" + std::to_string(helloPort()) + "/echo";
    std::string body = R"({"msg":"thunder_e2e_test"})";
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
    EXPECT_EQ(res, CURLE_OK) << "HTTP POST failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200L);
    EXPECT_NE(resp.find("thunder_e2e_test"), std::string::npos);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}
