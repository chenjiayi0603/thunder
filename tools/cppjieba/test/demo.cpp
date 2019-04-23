#include "cppjieba/Jieba.hpp"
#include "cppjieba/MPSegment.hpp"
#include "cppjieba/HMMSegment.hpp"
#include "cppjieba/MixSegment.hpp"
#include "cppjieba/KeywordExtractor.hpp"
#include "limonp/Colors.hpp"

using namespace std;
using namespace cppjieba;

const char* const DICT_PATH = "../dict/jieba.dict.utf8";
const char* const HMM_PATH = "../dict/hmm_model.utf8";
const char* const USER_DICT_PATH = "../dict/user.dict.utf8";
const char* const IDF_PATH = "../dict/idf.utf8";
const char* const STOP_WORD_PATH = "../dict/stop_words.utf8";

/*
 35w jieba.dict.utf8
 26w  idf.utf8
 34 hmm_model.utf8
 1500 stop_words.utf8

[demo] Cut With HMM 100000 times
他/来到/了/网易/杭研/大厦
[0.440 seconds]time consumed.

[demo] CutWithOutHMM 100000 times
他/来到/了/网易/杭/研/大厦
[0.280 seconds]time consumed.

[demo] CutWithOutHMM 100000 times
你/我/贷/，/国内/领先/的/在线/P/2/P/信用/投融资/平台/（/是/互联网/金融/ /I/T/F/I/N/ /产品/的/一种/）/，/金融/改革/践/行者/，/专注/中小/微/企业/、/个体户/及/农户/的/资金/发展/需求/，/长期/致力于/全民/信用/体系/的/构建/与/完善/。
[2.300 seconds]time consumed.

 [demo] CutForSearch 100000 times
小明/硕士/毕业/于/中国/科学/学院/科学院/中国科学院/计算/计算所/，/后/在/日本/京都/大学/日本京都大学/深造
[1.450 seconds]time consumed.

[demo] CutForSearch 10000 times
你/我/贷/，/国内/领先/的/在线/P2P/信用/融资/投融资/平台/（/是/互联/联网/互联网/金融/ /ITFIN/ /产品/的/一种/）/，/金融/改革/践/行者/，/专注/中小/微/企业/、/个体/个体户/及/农户/的/资金/发展/需求/，/长期/致力/致力于/全民/信用/体系/的/构建/与/完善/。
[0.400 seconds]time consumed.

//dfa use time(700.000000)ms,try num(10000)
 *
 * */
void CutWithHMM(cppjieba::Jieba& jieba,size_t times = 100000) {
	vector<string> words;
	string s;
	s = "他来到了网易杭研大厦";
	//cout << s << endl;
	//cout << "[demo] Cut With HMM" << endl;
	//cout << limonp::Join(words.begin(), words.end(), "/") << endl;
  long beginTime = clock();
  for (size_t i = 0; i < times; i ++) {
	  jieba.Cut(s, words, true);
  }
  printf("\n");
  long endTime = clock();
  cout << "[demo] Cut With HMM 100000 times" << endl;
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;
  ColorPrintln(GREEN, "[%.3lf seconds]time consumed.", double(endTime - beginTime)/CLOCKS_PER_SEC);
}
void CutWithOutHMM(cppjieba::Jieba& jieba,size_t times = 100000) {
	vector<string> words;
	string s;
	s = "你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。";
	//s = "他来到了网易杭研大厦";
	//cout << s << endl;
	//cout << "[demo] Cut With HMM" << endl;
	//cout << limonp::Join(words.begin(), words.end(), "/") << endl;
  long beginTime = clock();
  for (size_t i = 0; i < times; i ++) {
	  jieba.Cut(s, words, false);
  }
  printf("\n");
  long endTime = clock();
  cout << "[demo] CutWithOutHMM 100000 times" << endl;
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;
  ColorPrintln(GREEN, "[%.3lf seconds]time consumed.", double(endTime - beginTime)/CLOCKS_PER_SEC);
}


void CutForSearch(cppjieba::Jieba& jieba,size_t times = 10000)
{
	vector<string> words;
	string s;
	s = "你我贷，国内领先的在线P2P信用投融资平台（是互联网金融 ITFIN 产品的一种），金融改革践行者，专注中小微企业、个体户及农户的资金发展需求，长期致力于全民信用体系的构建与完善。";
	//"小明硕士毕业于中国科学院计算所，后在日本京都大学深造";
	cout << s << endl;
	cout << "[demo] CutForSearch" << endl;
	long beginTime = clock();
	for (size_t i = 0; i < times; i ++) {
		jieba.CutForSearch(s, words);
	}
	printf("\n");
	long endTime = clock();
	cout << "[demo] CutForSearch 10000 times" << endl;

	cout << limonp::Join(words.begin(), words.end(), "/") << endl;
	ColorPrintln(GREEN, "[%.3lf seconds]time consumed.", double(endTime - beginTime)/CLOCKS_PER_SEC);
}

int main(int argc, char** argv) {
  cppjieba::Jieba jieba(DICT_PATH,
        HMM_PATH,
        USER_DICT_PATH,
        IDF_PATH,
        STOP_WORD_PATH);

  CutWithHMM(jieba);
  CutWithOutHMM(jieba);
  CutForSearch(jieba);

  vector<string> words;
  vector<cppjieba::Word> jiebawords;
  string s;
  string result;

  s = "他来到了网易杭研大厦";
  cout << s << endl;
  cout << "[demo] Cut With HMM" << endl;
  jieba.Cut(s, words, true);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;

  cout << "[demo] Cut Without HMM " << endl;
  jieba.Cut(s, words, false);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;

  s = "我来到北京清华大学";
  cout << s << endl;
  cout << "[demo] CutAll" << endl;
  jieba.CutAll(s, words);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;

  s = "小明硕士毕业于中国科学院计算所，后在日本京都大学深造";
  cout << s << endl;
  cout << "[demo] CutForSearch" << endl;
  jieba.CutForSearch(s, words);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;

  cout << "[demo] Insert User Word" << endl;
  jieba.Cut("男默女泪", words);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;
  jieba.InsertUserWord("男默女泪");
  jieba.Cut("男默女泪", words);
  cout << limonp::Join(words.begin(), words.end(), "/") << endl;

  cout << "[demo] CutForSearch Word With Offset" << endl;
  jieba.CutForSearch(s, jiebawords, true);
  cout << jiebawords << endl;

  cout << "[demo] Lookup Tag for Single Token" << endl;
  const int DemoTokenMaxLen = 32;
  char DemoTokens[][DemoTokenMaxLen] = {"拖拉机", "CEO", "123", "。"};
  vector<pair<string, string> > LookupTagres(sizeof(DemoTokens) / DemoTokenMaxLen);
  vector<pair<string, string> >::iterator it;
  for (it = LookupTagres.begin(); it != LookupTagres.end(); it++) {
	it->first = DemoTokens[it - LookupTagres.begin()];
	it->second = jieba.LookupTag(it->first);
  }
  cout << LookupTagres << endl;

  cout << "[demo] Tagging" << endl;
  vector<pair<string, string> > tagres;
  s = "我是拖拉机学院手扶拖拉机专业的。不用多久，我就会升职加薪，当上CEO，走上人生巅峰。";
  jieba.Tag(s, tagres);
  cout << s << endl;
  cout << tagres << endl;

  cout << "[demo] Keyword Extraction" << endl;
  const size_t topk = 5;
  vector<cppjieba::KeywordExtractor::Word> keywordres;
  jieba.extractor.Extract(s, keywordres, topk);
  cout << s << endl;
  cout << keywordres << endl;
  return EXIT_SUCCESS;
}
