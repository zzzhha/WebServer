#include "AuthService.h"
#include <iostream>
#include <string>

// 测试辅助函数
void TestResult(const std::string& testName, bool expected, bool actual, int& passedTests) {
    if (expected == actual) {
        std::cout << "✅ " << testName << " 通过\n";
        passedTests++;
    } else {
        std::cout << "❌ " << testName << " 失败 - 期望: " << (expected ? "true" : "false") 
                  << ", 实际: " << (actual ? "true" : "false") << "\n";
    }
}

int main() {
    std::cout << "开始测试 AuthService...\n\n";
    
    int passedTests = 0;
    int totalTests = 0;
    
    // ============== 测试 ValidateUsername 函数 ==============
    std::cout << "1. 测试 ValidateUsername 函数:\n";
    
    // 有效的用户名
    TestResult("有效的用户名 'testuser'", true, AuthService::ValidateUsername("testuser"), passedTests);
    TestResult("有效的用户名 'test_user123'", true, AuthService::ValidateUsername("test_user123"), passedTests);
    totalTests += 2;
    
    // 无效的用户名 - 长度
    TestResult("太短的用户名 'ab'", false, AuthService::ValidateUsername("ab"), passedTests);
    TestResult("太长的用户名 'verylongusername1234567'", false, AuthService::ValidateUsername("verylongusername1234567"), passedTests);
    totalTests += 2;
    
    // 无效的用户名 - 字符
    TestResult("包含非法字符的用户名 'test@user'", false, AuthService::ValidateUsername("test@user"), passedTests);
    TestResult("包含空格的用户名 'test user'", false, AuthService::ValidateUsername("test user"), passedTests);
    totalTests += 2;
    
    // 无效的用户名 - 首字符
    TestResult("以数字开头的用户名 '123user'", false, AuthService::ValidateUsername("123user"), passedTests);
    TestResult("以下划线开头的用户名 '_testuser'", false, AuthService::ValidateUsername("_testuser"), passedTests);
    totalTests += 2;
    
    // ============== 测试 ValidatePassword 函数 ==============
    std::cout << "\n2. 测试 ValidatePassword 函数:\n";
    
    TestResult("有效的密码 'password123'", true, AuthService::ValidatePassword("password123"), passedTests);
    TestResult("6位密码 '123456'", true, AuthService::ValidatePassword("123456"), passedTests);
    TestResult("太短的密码 '12345'", false, AuthService::ValidatePassword("12345"), passedTests);
    TestResult("空密码", false, AuthService::ValidatePassword(""), passedTests);
    totalTests += 4;
    
    // ============== 测试 HandleRegister 函数 ==============
    std::cout << "\n3. 测试 HandleRegister 函数:\n";
    
    // 注意：这些测试会实际操作数据库，可能需要清理测试数据
    std::string testUsername = "unittest_user";
    std::string testPassword = "password123";
    
    // 清理可能存在的测试用户
    UserDao::DeleteUser(testUsername);
    
    // 测试注册流程
    TestResult("有效的注册请求", true, AuthService::HandleRegister(testUsername, testPassword), passedTests);
    TestResult("注册已存在的用户", false, AuthService::HandleRegister(testUsername, testPassword), passedTests);
    TestResult("空用户名注册", false, AuthService::HandleRegister("", testPassword), passedTests);
    TestResult("空密码注册", false, AuthService::HandleRegister("testuser2", ""), passedTests);
    TestResult("无效用户名注册", false, AuthService::HandleRegister("123", testPassword), passedTests);
    TestResult("无效密码注册", false, AuthService::HandleRegister("testuser3", "12345"), passedTests);
    totalTests += 6;
    
    // ============== 测试 HandleLogin 函数 ==============
    std::cout << "\n4. 测试 HandleLogin 函数:\n";
    
    TestResult("有效的登录请求", true, AuthService::HandleLogin(testUsername, testPassword), passedTests);
    TestResult("空用户名登录", false, AuthService::HandleLogin("", testPassword), passedTests);
    TestResult("空密码登录", false, AuthService::HandleLogin(testUsername, ""), passedTests);
    TestResult("用户不存在登录", false, AuthService::HandleLogin("nonexistent_user", testPassword), passedTests);
    TestResult("密码错误登录", false, AuthService::HandleLogin(testUsername, "wrongpassword"), passedTests);
    totalTests += 5;
    
    // 清理测试数据
    UserDao::DeleteUser(testUsername);
    
    // ============== 测试总结 ==============
    std::cout << "\n============== 测试总结 ==============\n";
    std::cout << "总测试数: " << totalTests << "\n";
    std::cout << "通过测试数: " << passedTests << "\n";
    std::cout << "失败测试数: " << (totalTests - passedTests) << "\n";
    
    if (totalTests == passedTests) {
        std::cout << "\n🎉 所有测试通过！\n";
        return 0;
    } else {
        std::cout << "\n❌ 部分测试失败！\n";
        return 1;
    }
}