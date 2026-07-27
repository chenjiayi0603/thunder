#include <gtest/gtest.h>
#include <string>
#include <sstream>
#include <vector>

static bool TopicMatches(const std::string& filter, const std::string& topic) {
    auto split=[](const std::string& s)->std::vector<std::string>{
        std::vector<std::string> p; if(s.empty()) return p;
        size_t st=0; if(s[0]=='/') st=1;
        std::stringstream ss(s.substr(st)); std::string part;
        while(std::getline(ss,part,'/')) p.push_back(part);
        return p;
    };
    auto fp=split(filter), tp=split(topic);
    size_t fi=0,ti=0;
    while(fi<fp.size()){if(fp[fi]=="#")return true;if(ti>=tp.size())return false;
        if(fp[fi]!="+"&&fp[fi]!=tp[ti])return false;++fi;++ti;}
    return ti==tp.size();
}

TEST(TopicMatch, Exact) { EXPECT_TRUE(TopicMatches("foo","foo")); EXPECT_TRUE(TopicMatches("a/b","a/b")); EXPECT_FALSE(TopicMatches("foo","bar")); }
TEST(TopicMatch, Plus) { EXPECT_TRUE(TopicMatches("+","foo")); EXPECT_TRUE(TopicMatches("+/bar","foo/bar")); EXPECT_TRUE(TopicMatches("home/+/temp","home/kitchen/temp")); EXPECT_FALSE(TopicMatches("+","foo/bar")); }
TEST(TopicMatch, Hash) { EXPECT_TRUE(TopicMatches("#","foo")); EXPECT_TRUE(TopicMatches("foo/#","foo/bar/baz")); EXPECT_FALSE(TopicMatches("a/#","b/c")); }
TEST(TopicMatch, Combine) { EXPECT_TRUE(TopicMatches("+/bar/#","foo/bar/baz/qux")); }
TEST(TopicMatch, IoT) { EXPECT_TRUE(TopicMatches("sensors/+/temp","sensors/A/temp")); EXPECT_TRUE(TopicMatches("building/1/#","building/1/room/light")); EXPECT_FALSE(TopicMatches("building/1/#","building/2/light")); }
TEST(TopicMatch, RootHash) { EXPECT_TRUE(TopicMatches("#","device/status")); }
TEST(TopicMatch, Empty) { EXPECT_FALSE(TopicMatches("a/b","a")); EXPECT_FALSE(TopicMatches("a","a/b")); }
TEST(TopicMatch, NoMatch) { EXPECT_FALSE(TopicMatches("+/b","c/d")); }
