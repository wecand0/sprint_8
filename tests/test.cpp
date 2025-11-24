#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <streambuf>
#include <string>

using namespace std::string_literals;
namespace fs = std::filesystem;

// helper function for wirte content to file
void write_file(const fs::path &path, const std::string &content) { std::ofstream(path) << content; }

// helper function for getting content from file
std::string get_file_contents(const fs::path &path) {
    std::ifstream file(path);
    std::string content(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
    return content;
}

std::string run_refactor_tool(const fs::path &path) {
    auto cmd = "./refactor_tool "s + path.string() + " --"s;
    int rc = system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "failed to run refactor_tool";

    auto content = get_file_contents(path);
    fs::remove(path);

    return content;
}

// helper function for getting refactored content from test file
std::string get_refactored_file_contents(const std::string &test_name) {
    auto src_file = fs::path{"../tests/tests_data/"s + test_name + ".cpp"s};
    auto tmp_file = fs::path{"../tests/tests_data/tmp/"s + test_name + "_tmp.cpp"s};
    fs::copy_file(src_file, tmp_file, fs::copy_options::overwrite_existing);

    return run_refactor_tool(tmp_file);
}

// helper function for getting refactored content
std::string get_refactored_contents(const std::string &content) {
    auto tmp_file = fs::path{"../tests/tests_data/tmp/tmp.cpp"s};
    write_file(tmp_file, content);
    return run_refactor_tool(tmp_file);
}

class refactor_tool : public testing::TestWithParam<std::string> {};

TEST_P(refactor_tool, test) {
    const auto test_name = GetParam();
    const auto ref_file = fs::path{"../tests/tests_data/"s + test_name + "_ref.cpp"s};

    const auto actual = get_file_contents(ref_file);
    const auto expected = get_refactored_file_contents(test_name);

    EXPECT_EQ(actual, expected);
}

INSTANTIATE_TEST_SUITE_P(test, refactor_tool, testing::Values("test1"s, "test2"s, "test3"s));

TEST(refactor_tool_ext, nv_dtor_positive) {
    const auto testcode = "struct Base { ~Base(); }; "
                          "struct Derived : Base {};"s;
    const auto expected = "struct Base { virtual ~Base(); }; "
                          "struct Derived : Base {};"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}

TEST(refactor_tool_ext, nv_dtor_negative) {
    const auto testcode = "struct BaseA { ~BaseA(); }; "
                          "struct BaseB {};"s;
    const auto expected = "struct BaseA { ~BaseA(); }; "
                          "struct BaseB {};"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}

TEST(refactor_tool_ext, miss_override_positive) {
    const auto testcode = "struct Base { "
                          "  virtual ~Base(); "
                          "  virtual void f();"
                          "}; "
                          "struct Derived : Base { "
                          "  ~Derived(); "
                          "  void f();"
                          "};"s;
    const auto expected = "struct Base { "
                          "  virtual ~Base(); "
                          "  virtual void f();"
                          "}; "
                          "struct Derived : Base { "
                          "  ~Derived(); "
                          "  void f() override;"
                          "};"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}

TEST(refactor_tool_ext, miss_override_negative) {
    const auto testcode = "struct BaseA { virtual ~BaseA(); }; "
                          "struct BaseB { ~BaseB(); };"s;
    const auto expected = "struct BaseA { virtual ~BaseA(); }; "
                          "struct BaseB { ~BaseB(); };"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}

TEST(refactor_tool_ext, crange_for_positive) {
    const auto testcode = "void f() { "
                          "  using big_item_t = int[1'000'000]; "
                          "  big_item_t big_items[100]; "
                          "  for (const auto big_item : big_items) {} "
                          "}"s;
    const auto expected = "void f() { "
                          "  using big_item_t = int[1'000'000]; "
                          "  big_item_t big_items[100]; "
                          "  for (const auto& big_item : big_items) {} "
                          "}"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}

TEST(refactor_tool_ext, crange_for_negative) {
    const auto testcode = "void f() { "
                          "  using big_item_t = int[1'000'000]; "
                          "  big_item_t big_items[100]; "
                          "  for (const auto &big_item : big_items) {} "
                          "}"s;
    const auto expected = "void f() { "
                          "  using big_item_t = int[1'000'000]; "
                          "  big_item_t big_items[100]; "
                          "  for (const auto &big_item : big_items) {} "
                          "}"s;
    const auto actual = get_refactored_contents(testcode);
    EXPECT_EQ(actual, expected);
}
