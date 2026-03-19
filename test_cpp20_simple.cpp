#include <iostream>

int main() {
    std::cout << "测试 C++20 功能" << std::endl;
    
    // 测试 C++20 特性
    #if __cplusplus >= 202002L
        std::cout << "编译器支持 C++20 标准" << std::endl;
        std::cout << "__cplusplus = " << __cplusplus << std::endl;
    #else
        std::cout << "编译器不支持 C++20 标准" << std::endl;
        std::cout << "__cplusplus = " << __cplusplus << std::endl;
    #endif
    
    return 0;
}