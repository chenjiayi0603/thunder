/**
 * CJsonObject JSON 工具单元测试
 */
#include "util/json/CJsonObject.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(CJsonObject, DefaultConstructionEmpty)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.IsEmpty());
    EXPECT_FALSE(o.IsArray());
    EXPECT_TRUE(o.IsEmpty());
}

TEST(CJsonObject, ParseValidJson)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Parse(R"({"key":"value"})"));
    EXPECT_FALSE(o.IsEmpty());
}

TEST(CJsonObject, ParseInvalidJsonReturnsFalse)
{
    util::CJsonObject o;
    EXPECT_FALSE(o.Parse("not json"));
    EXPECT_TRUE(o.IsEmpty());
}

TEST(CJsonObject, ParseEmptyObject)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Parse("{}"));
    EXPECT_FALSE(o.IsEmpty());
}

TEST(CJsonObject, StringConstructor)
{
    util::CJsonObject o(R"({"a":1})");
    int v = 0;
    EXPECT_TRUE(o.Get("a", v));
    EXPECT_EQ(1, v);
}

TEST(CJsonObject, CopyConstructor)
{
    util::CJsonObject o1(R"({"x":42})");
    util::CJsonObject o2(o1);
    int v = 0;
    EXPECT_TRUE(o2.Get("x", v));
    EXPECT_EQ(42, v);
}

TEST(CJsonObject, AssignmentOperator)
{
    util::CJsonObject o1(R"({"y":99})");
    util::CJsonObject o2;
    o2 = o1;
    int v = 0;
    EXPECT_TRUE(o2.Get("y", v));
    EXPECT_EQ(99, v);
}

TEST(CJsonObject, EqualityOperator)
{
    util::CJsonObject o1(R"({"k":"v"})");
    util::CJsonObject o2(R"({"k":"v"})");
    EXPECT_TRUE(o1 == o2);
}

TEST(CJsonObject, InequalityOperator)
{
    util::CJsonObject o1(R"({"k":"a"})");
    util::CJsonObject o2(R"({"k":"b"})");
    EXPECT_FALSE(o1 == o2);
}

TEST(CJsonObject, GetString)
{
    util::CJsonObject o(R"({"name":"thunder"})");
    std::string name;
    EXPECT_TRUE(o.Get("name", name));
    EXPECT_EQ("thunder", name);
}

TEST(CJsonObject, GetInt32)
{
    util::CJsonObject o(R"({"count":123})");
    int32_t v = 0;
    EXPECT_TRUE(o.Get("count", v));
    EXPECT_EQ(123, v);
}

TEST(CJsonObject, GetUint32)
{
    util::CJsonObject o(R"({"port":8080})");
    uint32 v = 0;
    EXPECT_TRUE(o.Get("port", v));
    EXPECT_EQ(8080u, v);
}

TEST(CJsonObject, GetInt64)
{
    util::CJsonObject o(R"({"big":9223372036854775807})");
    int64 v = 0;
    EXPECT_TRUE(o.Get("big", v));
}

TEST(CJsonObject, GetBool)
{
    util::CJsonObject o(R"({"enabled":true})");
    bool b = false;
    EXPECT_TRUE(o.Get("enabled", b));
    EXPECT_TRUE(b);
}

TEST(CJsonObject, GetFloat)
{
    util::CJsonObject o(R"({"pi":3.14})");
    float f = 0.0f;
    EXPECT_TRUE(o.Get("pi", f));
    EXPECT_NEAR(3.14f, f, 0.01f);
}

TEST(CJsonObject, GetDouble)
{
    util::CJsonObject o(R"({"e":2.718281828})");
    double d = 0.0;
    EXPECT_TRUE(o.Get("e", d));
    EXPECT_NEAR(2.718, d, 0.001);
}

TEST(CJsonObject, GetMissingKey)
{
    util::CJsonObject o(R"({"a":1})");
    std::string s;
    EXPECT_FALSE(o.Get("nonexistent", s));
}

TEST(CJsonObject, GetNestedObject)
{
    util::CJsonObject o(R"({"inner":{"x":10}})");
    util::CJsonObject inner;
    EXPECT_TRUE(o.Get("inner", inner));
    int x = 0;
    EXPECT_TRUE(inner.Get("x", x));
    EXPECT_EQ(10, x);
}

TEST(CJsonObject, AddString)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("key", std::string("value")));
    std::string out;
    EXPECT_TRUE(o.Get("key", out));
    EXPECT_EQ("value", out);
}

TEST(CJsonObject, AddInt32)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("num", 42));
    int v = 0;
    EXPECT_TRUE(o.Get("num", v));
    EXPECT_EQ(42, v);
}

TEST(CJsonObject, AddBool)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("flag", true, true));
    bool b = false;
    EXPECT_TRUE(o.Get("flag", b));
    EXPECT_TRUE(b);
}

TEST(CJsonObject, ReplaceString)
{
    util::CJsonObject o(R"({"msg":"old"})");
    EXPECT_TRUE(o.Replace("msg", std::string("new")));
    std::string out;
    EXPECT_TRUE(o.Get("msg", out));
    EXPECT_EQ("new", out);
}

TEST(CJsonObject, DeleteExistingKey)
{
    util::CJsonObject o(R"({"a":1,"b":2})");
    EXPECT_TRUE(o.Delete("a"));
    int v = 0;
    EXPECT_FALSE(o.Get("a", v));
}

TEST(CJsonObject, DeleteNonExistingKey)
{
    util::CJsonObject o(R"({"x":1})");
    EXPECT_TRUE(o.Delete("y"));
}

TEST(CJsonObject, ArrayParse)
{
    util::CJsonObject arr;
    EXPECT_TRUE(arr.Parse("[1,2,3]"));
    EXPECT_TRUE(arr.IsArray());
    EXPECT_EQ(3, arr.GetArraySize());
}

TEST(CJsonObject, ArrayGetValue)
{
    util::CJsonObject arr;
    arr.Parse("[10,20,30]");
    int v = 0;
    EXPECT_TRUE(arr.Get(0, v)); EXPECT_EQ(10, v);
    EXPECT_TRUE(arr.Get(1, v)); EXPECT_EQ(20, v);
    EXPECT_TRUE(arr.Get(2, v)); EXPECT_EQ(30, v);
}

TEST(CJsonObject, ArrayGetOutOfBounds)
{
    util::CJsonObject arr;
    arr.Parse("[1]");
    int v = 0;
    EXPECT_FALSE(arr.Get(5, v));
}

TEST(CJsonObject, ArrayAddString)
{
    util::CJsonObject arr;
    arr.Parse("[]");
    EXPECT_TRUE(arr.Add(std::string("hello")));
    EXPECT_EQ(1, arr.GetArraySize());
}

TEST(CJsonObject, ArrayAddInt)
{
    util::CJsonObject arr;
    arr.Parse("[]");
    arr.Add(42); arr.Add(100);
    EXPECT_EQ(2, arr.GetArraySize());
    int v = 0;
    EXPECT_TRUE(arr.Get(0, v)); EXPECT_EQ(42, v);
}

TEST(CJsonObject, ArrayDelete)
{
    util::CJsonObject arr;
    arr.Parse("[1,2,3,4]");
    arr.Delete(1);
    EXPECT_EQ(3, arr.GetArraySize());
}

TEST(CJsonObject, NestedObjectAdd)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.AddEmptySubObject("config"));
}

TEST(CJsonObject, NestedArrayAdd)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.AddEmptySubArray("items"));
}

TEST(CJsonObject, GetKeys)
{
    util::CJsonObject o(R"({"a":1,"b":2,"c":3})");
    std::vector<std::string> keys;
    o.GetKeys(keys);
    EXPECT_EQ(3u, keys.size());
}

TEST(CJsonObject, ToFormattedString)
{
    util::CJsonObject o(R"({"a":1})");
    std::string fmt = o.ToFormattedString();
    EXPECT_NE(std::string::npos, fmt.find("a"));
}

TEST(CJsonObject, ErrorMessageOnParse)
{
    util::CJsonObject o;
    o.Parse("bad json");
    EXPECT_FALSE(o.GetErrMsg().empty());
}

TEST(CJsonObject, ObjectOperatorParen)
{
    util::CJsonObject o(R"({"code":200})");
    EXPECT_EQ("200", o("code"));
}

TEST(CJsonObject, GetUint64)
{
    util::CJsonObject o(R"({"big":18446744073709551615})");
    uint64 v = 0;
    EXPECT_TRUE(o.Get("big", v));
    EXPECT_EQ(18446744073709551615ULL, v);
}

TEST(CJsonObject, AddAndGetUint32)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("port", (uint32)65535));
    uint32 v = 0;
    EXPECT_TRUE(o.Get("port", v));
    EXPECT_EQ(65535u, v);
}

TEST(CJsonObject, AddAndGetFloat)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("pi", 3.14f));
    float f = 0;
    EXPECT_TRUE(o.Get("pi", f));
    EXPECT_NEAR(3.14f, f, 0.001f);
}

TEST(CJsonObject, AddAndGetDouble)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Add("e", 2.718281828));
    double d = 0;
    EXPECT_TRUE(o.Get("e", d));
    EXPECT_NEAR(2.718281828, d, 1e-9);
}

TEST(CJsonObject, ReplaceInt32)
{
    util::CJsonObject o(R"({"n":1})");
    EXPECT_TRUE(o.Replace("n", (int32)99));
    int32 v = 0;
    EXPECT_TRUE(o.Get("n", v));
    EXPECT_EQ(99, v);
}

TEST(CJsonObject, ReplaceBool)
{
    util::CJsonObject o(R"({"flag":false})");
    EXPECT_TRUE(o.Replace("flag", true, true));
    bool b = false;
    EXPECT_TRUE(o.Get("flag", b));
    EXPECT_TRUE(b);
}

TEST(CJsonObject, ReplaceDouble)
{
    util::CJsonObject o(R"({"val":1.0})");
    EXPECT_TRUE(o.Replace("val", 3.14));
    double d = 0;
    EXPECT_TRUE(o.Get("val", d));
    EXPECT_NEAR(3.14, d, 1e-9);
}

TEST(CJsonObject, GetAsStringOnPlainString)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Parse(R"("hello")"));
    std::string s;
    EXPECT_TRUE(o.GetAsString(s));
    EXPECT_EQ("hello", s);
}

TEST(CJsonObject, GetAsStringOnObject)
{
    util::CJsonObject o(R"({"a":1})");
    std::string s;
    EXPECT_FALSE(o.GetAsString(s));
}

TEST(CJsonObject, IsEmpty)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.IsEmpty());
    o.Parse(R"({"a":1})");
    EXPECT_FALSE(o.IsEmpty());
}

TEST(CJsonObject, IsArray)
{
    util::CJsonObject o(R"([1,2,3])");
    EXPECT_TRUE(o.IsArray());
    util::CJsonObject o2(R"({"a":1})");
    EXPECT_FALSE(o2.IsArray());
}

TEST(CJsonObject, IsNull)
{
    util::CJsonObject o;
    EXPECT_TRUE(o.Parse("null"));
    EXPECT_TRUE(o.IsNull());
}

TEST(CJsonObject, AddAsFirstString)
{
    util::CJsonObject arr;
    arr.Add(std::string("b"));
    arr.AddAsFirst(std::string("a"));
    std::string s;
    EXPECT_TRUE(arr.Get(0, s));
    EXPECT_EQ("a", s);
}

TEST(CJsonObject, SubObjectBracketOperator)
{
    util::CJsonObject o(R"({"inner":{"x":42}})");
    int32 v = 0;
    EXPECT_TRUE(o["inner"].Get("x", v));
    EXPECT_EQ(42, v);
}

TEST(CJsonObject, RoundtripComplexJson)
{
    std::string json = R"({"code":0,"msg":"ok","list":[1,2,3],"nested":{"k":"v"}})";
    util::CJsonObject o(json);
    // Verify round-trip parse → serialize preserves content
    util::CJsonObject o2(o.ToString());
    int32 code = -1;
    std::string msg;
    EXPECT_TRUE(o2.Get("code", code));
    EXPECT_EQ(0, code);
    EXPECT_TRUE(o2.Get("msg", msg));
    EXPECT_EQ("ok", msg);
    EXPECT_EQ(3, o2["list"].GetArraySize());
}

TEST(CJsonObject, ArrayReplaceInt)
{
    util::CJsonObject arr;
    arr.Parse("[1,2,3]");
    EXPECT_TRUE(arr.Replace(1, (int32)99));
    int32 v = 0;
    EXPECT_TRUE(arr.Get(1, v));
    EXPECT_EQ(99, v);
}

TEST(CJsonObject, AddSubObjectByValue)
{
    util::CJsonObject inner(R"({"x":1})");
    util::CJsonObject outer;
    EXPECT_TRUE(outer.Add("sub", inner));
    int32 v = 0;
    EXPECT_TRUE(outer["sub"].Get("x", v));
    EXPECT_EQ(1, v);
}
