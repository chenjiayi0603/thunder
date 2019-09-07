#include <iostream>
#include <vector>
#include <string.h>//strlen()
#include <typeinfo>//typeid().name()
#include <iterator>
#include <ctime>
#include "threadpool.h"

using namespace std;

class CNoMovePerson
{
public:
    static size_t DCtor;
    static size_t Ctor;
    static size_t CCtor;
    static size_t CAsgn;
    static size_t MCtor;
    static size_t MAsgn;
    static size_t Dtor;
private:
    int _age;
    char* _name;
    size_t _len;
    void _test_name(const char *s)
    {
        _name = new char[_len+1];
        memcpy(_name, s, _len);
        _name[_len] = '\0';
    }

public:
    //default ctor
    CNoMovePerson(): _age(0) , _name(NULL), _len(0){DCtor++;}

    CNoMovePerson(const int age, const char * p) : _age(age), _len(strlen(p)) {
        _test_name(p);
        Ctor++;
    }

    //dctor
    ~CNoMovePerson(){
        if(_name){
            delete _name;
        }
        Dtor++;
    }

    // copy ctor
    CNoMovePerson (const CNoMovePerson& p):_age(p._age),_len(p._len){
        _test_name(p._name);
        CCtor++;}

    //copy assignment
    CNoMovePerson & operator=(const CNoMovePerson& p)
    {
        if (this != &p){
            if(_name) delete _name;
            _len = p._len;
            _age = p._age;
            _test_name(p._name);
        }
        else{
            cout<< "self Assignment. Nothing to do." <<endl;
        }
        CAsgn++;
        return *this;
    }
};

size_t CNoMovePerson::DCtor = 0;
size_t CNoMovePerson::Ctor  = 0;
size_t CNoMovePerson::CCtor = 0;
size_t CNoMovePerson::CAsgn = 0;
size_t CNoMovePerson::MCtor = 0;
size_t CNoMovePerson::MAsgn = 0;
size_t CNoMovePerson::Dtor  = 0;


class Person
{

public:
    static size_t DCtor;
    static size_t Ctor;
    static size_t CCtor;
    static size_t CAsgn;
    static size_t MCtor;
    static size_t MAsgn;
    static size_t Dtor;


    int _age;
    char* _name;
    size_t _len;
private:
    void _test_name(const char *s)
    {
        _name = new char[_len+1];
        memcpy(_name, s, _len);
        _name[_len] = '\0';
    }

public:
    //default ctor
    Person(): _age(0) , _name(NULL), _len(0){ DCtor++;cout<< "default ctor" <<endl;}

    Person(const int age, const char * p) : _age(age), _len(strlen(p)) {
        _test_name(p);
        Ctor++;
    }

    //dctor
    ~Person(){
        if(_name){
            delete _name;
        }
        Dtor++;
        cout<< "dctor" <<endl;
    }

    // copy ctor
    Person (const Person& p):_age(p._age),_len(p._len){
        _test_name(p._name);
        CCtor++;
        cout<< "copy ctor" <<endl;
    }

    //copy assignment
    Person & operator=(const Person& p)
    {
        if (this != &p){
            if(_name) delete _name;
            _len = p._len;
            _age = p._age;
            _test_name(p._name);
        }
        else{
            cout<< "self Assignment. Nothing to do." <<endl;
        }
        CAsgn++;
        cout<< "copy assignment" <<endl;
        return *this;
    }

    // move cotr , wihth "noexcept"
    Person(Person&& p) noexcept :_age(p._age) , _name(p._name), _len(p._len){
        MCtor++;
        p._age = 0;
        p._name = NULL;//必须为NULL 如果你把这里设为空 那么这个函数走完之后将调用析够函数 因为当前的Person类 和你将要析够的Person的_name指向同一部分 析构部分见析构函数
        cout<< "move cotr" <<endl;
    }
    // move assignment
    Person& operator=(Person&& p) noexcept {

        if (this != &p)
        {
            if(_name) delete _name;
            _age  = p._age;
            _len  = p._len;
            _name = p._name;
            p._age  = 0;
            p._len  = 0;
            p._name = NULL;
        }
        MAsgn++;
        cout<< "move assignment" <<endl;
        return *this;
    }
};

size_t Person::DCtor = 0;
size_t Person::Ctor  = 0;
size_t Person::CCtor = 0;
size_t Person::CAsgn = 0;
size_t Person::MCtor = 0;
size_t Person::MAsgn = 0;
size_t Person::Dtor  = 0;

template<typename T>
void output_Static_data(const T& myPerson)
{
    cout << typeid(myPerson).name() << "--" << endl;
    cout << "CCtor=" << T::CCtor <<endl
         << "MCtor=" << T::MCtor <<endl
         << "CAsgn=" << T::CAsgn <<endl
         << "MAsgn=" << T::MAsgn <<endl
         << "Dtor="  << T::Dtor  <<endl
         << "Ctor="  << T::Ctor  <<endl
         << "DCtor=" << T::DCtor <<endl
         << endl;
}

template<typename M, typename NM>
void test_moveable(M c1, NM c2, long& value)
{
    char buf[10];

    typedef typename iterator_traits<typename M::iterator>::value_type MyPerson;

    clock_t timeStart = clock();
    for(long i=0; i<value; i++)
    {
        snprintf(buf,10,"%d",rand());
        auto ite = c1.end();
        c1.insert(ite,MyPerson(0,buf));
    }
    cout << "Move Person" << endl;
    cout << "construction, milli-seconds: "<<(clock()-timeStart) << endl;
    cout << "size()= " << c1.size() << endl;

    output_Static_data(*c1.begin());

    timeStart = clock();
    M c11(c1);
    cout << "copy, milli-seconds: "<<(clock()-timeStart) << endl;

    timeStart = clock();
    M c12(std::move(c1));
    cout << "move construction, milli-seconds: "<<(clock()-timeStart) << endl;

    timeStart = clock();
    c11.swap(c12);
    cout << "swap, milli-seconds: "<<(clock()-timeStart) << endl;


    cout << "------------------------------" << endl;
    cout << "No Move Person" << endl;

    typedef typename iterator_traits<typename NM::iterator>::value_type MyPersonNoMove;

    timeStart = clock();
    for(long i=0; i<value; i++)
    {
        snprintf(buf,10,"%d",rand());
        auto ite = c2.end();
        c2.insert(ite,MyPersonNoMove(0,buf));
    }

    cout << "construction, milli-seconds: "<<(clock()-timeStart) << endl;
    cout << "size()= " << c2.size() << endl;

    output_Static_data(*c1.begin());

    timeStart = clock();
    NM c22(c2);
    cout << "move copy, milli-seconds: "<<(clock()-timeStart) << endl;

    timeStart = clock();
    NM c222(std::move(c2));
    cout << "move construction, milli-seconds: "<<(clock()-timeStart) << endl;

    timeStart = clock();
    c22.swap(c222);
    cout << "swap, milli-seconds: "<<(clock()-timeStart) << endl;
}

long value = 3000000;


void fun1(Person& slp)//只能用拷贝
{
	printf("  hello, fun1 !  %u. name:%s,%p\n" ,std::this_thread::get_id(),slp._name,slp._name);
	if (1) {
		printf(" ======= fun1 sleep %d  =========  %d\n",1, std::this_thread::get_id());
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		//Sleep(slp );
	}
}

int main()
{
//    test_moveable(vector<Person>(),vector<CNoMovePerson>(),value);
	std::threadpool executor{ 50 };
	Person persion(0,"12345");
	printf("  main!  %u. name:%s,%p\n" ,std::this_thread::get_id(),persion._name,persion._name);
	std::future<void> ff = executor.commit(fun1,persion);
	return 0;
}
/*
Move Person
construction, milli-seconds: 1980000
size()= 3000000
6Person--
CCtor=0
MCtor=7194303
CAsgn=0
MAsgn=0
Dtor=7194303
Ctor=3000000
DCtor=0

copy, milli-seconds: 980000
move construction, milli-seconds: 0
swap, milli-seconds: 0
------------------------------
No Move Person
construction, milli-seconds: 5070000
size()= 3000000
6Person--
CCtor=3000000
MCtor=7194303
CAsgn=0
MAsgn=0
Dtor=7194303
Ctor=3000000
DCtor=0

move copy, milli-seconds: 1760000
move construction, milli-seconds: 0
swap, milli-seconds: 0
 * */
