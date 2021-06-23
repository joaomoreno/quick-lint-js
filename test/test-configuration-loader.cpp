// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <quick-lint-js/configuration-loader.h>
#include <quick-lint-js/configuration.h>
#include <quick-lint-js/fake-configuration-filesystem.h>
#include <quick-lint-js/file-canonical.h>
#include <quick-lint-js/file-matcher.h>
#include <quick-lint-js/file-path.h>
#include <quick-lint-js/file.h>
#include <quick-lint-js/options.h>
#include <quick-lint-js/temporary-directory.h>
#include <quick-lint-js/warning.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if QLJS_HAVE_STD_FILESYSTEM
#include <filesystem>
#endif

QLJS_WARNING_IGNORE_GCC("-Wmissing-field-initializers")

#define EXPECT_DEFAULT_CONFIG(config)                                  \
  do {                                                                 \
    EXPECT_TRUE((config).globals().find(u8"Array"sv));                 \
    EXPECT_TRUE((config).globals().find(u8"console"sv));               \
    EXPECT_FALSE((config).globals().find(u8"variableDoesNotExist"sv)); \
  } while (false)

using ::testing::AnyOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using namespace std::literals::string_view_literals;

namespace quick_lint_js {
namespace {
void move_file(const std::string& from, const std::string& to);

class test_configuration_loader : public ::testing::Test {
 public:
  std::string make_temporary_directory() {
    std::string temp_dir = quick_lint_js::make_temporary_directory();
    this->temporary_directories_.emplace_back(temp_dir);
    return temp_dir;
  }

  void set_current_working_directory(const std::string& path) {
    this->set_current_working_directory(path.c_str());
  }

  void set_current_working_directory(const char* path) {
    if (!this->old_working_directory_.has_value()) {
      this->old_working_directory_ = get_current_working_directory();
    }
    quick_lint_js::set_current_working_directory(path);
  }

 protected:
  void TearDown() override {
    if (this->old_working_directory_.has_value()) {
      set_current_working_directory(*this->old_working_directory_);
    }
    for (const std::string& temp_dir : this->temporary_directories_) {
      delete_directory_recursive(temp_dir);
    }
  }

 private:
  std::vector<std::string> temporary_directories_;
  std::optional<std::string> old_working_directory_;
};

TEST_F(test_configuration_loader,
       file_with_no_config_file_gets_default_config) {
  // NOTE(strager): This test assumes that there is no quick-lint-js.config file
  // in /tmp or in /.
  std::string temp_dir = this->make_temporary_directory();

  configuration_loader loader(basic_configuration_filesystem::instance());
  std::string js_file = temp_dir + "/hello.js";
  write_file(js_file, u8""sv);
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = js_file.c_str(),
      .config_file = nullptr,
  });
  EXPECT_DEFAULT_CONFIG(*config);
}

TEST_F(test_configuration_loader, find_quick_lint_js_config_in_same_directory) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({})"sv);

  std::string js_file = temp_dir + "/hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       find_dot_quick_lint_js_config_in_same_directory) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/.quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  std::string js_file = temp_dir + "/hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       quick_lint_js_config_is_used_over_dot_quick_lint_js_config) {
  std::string temp_dir = this->make_temporary_directory();
  write_file(temp_dir + "/quick-lint-js.config", u8"{}"sv);
  write_file(temp_dir + "/.quick-lint-js.config", u8"{}"sv);

  std::string js_file = temp_dir + "/hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(),
                   temp_dir + "/quick-lint-js.config");
}

TEST_F(test_configuration_loader,
       find_config_in_same_directory_of_relative_path) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  std::string config_file = "quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  std::string js_file = "hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, quick_lint_js_config_directory_fails) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/" + config_file_name;
    create_directory(config_file);

    std::string js_file = temp_dir + "/hello.js";
    write_file(js_file, u8""sv);
    configuration_loader loader(basic_configuration_filesystem::instance());
    configuration_or_error config = loader.load_for_file(js_file);

    EXPECT_FALSE(config.ok());
    EXPECT_THAT(config.error,
                HasSubstr(canonicalize_path(config_file).c_str()));
    EXPECT_THAT(
        config.error,
        AnyOf(HasSubstr("Is a directory"),
              HasSubstr(
                  "Access is denied")  // TODO(strager): Improve this message.
              ));
  }
}

TEST_F(test_configuration_loader, find_config_in_parent_directory) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);
    std::string temp_dir = this->make_temporary_directory();
    create_directory(temp_dir + "/dir");
    std::string config_file = temp_dir + "/" + config_file_name;
    write_file(config_file, u8"{}"sv);

    std::string js_file = temp_dir + "/dir/hello.js";
    write_file(js_file, u8""sv);
    configuration_loader loader(basic_configuration_filesystem::instance());
    configuration_or_error config = loader.load_for_file(js_file);

    EXPECT_SAME_FILE(config->config_file_path(), config_file);
  }
}

TEST_F(test_configuration_loader,
       find_config_in_parent_directory_of_relative_path) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  create_directory("dir");
  std::string config_file = "quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  std::string js_file = "dir/hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, find_config_in_parent_directory_of_cwd) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory(temp_dir + "/dir");
  this->set_current_working_directory(temp_dir + "/dir");
  std::string config_file = "../quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  std::string js_file = "hello.js";
  write_file(js_file, u8""sv);
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, find_config_in_ancestor_directory) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);
    std::string temp_dir = this->make_temporary_directory();
    create_directory(temp_dir + "/a");
    create_directory(temp_dir + "/a/b");
    create_directory(temp_dir + "/a/b/c");
    create_directory(temp_dir + "/a/b/c/d");
    create_directory(temp_dir + "/a/b/c/d/e");
    create_directory(temp_dir + "/a/b/c/d/e/f");
    std::string config_file = temp_dir + "/" + config_file_name;
    write_file(config_file, u8"{}"sv);

    std::string js_file = temp_dir + "/a/b/c/d/e/f/hello.js";
    write_file(js_file, u8""sv);
    configuration_loader loader(basic_configuration_filesystem::instance());
    configuration_or_error config = loader.load_for_file(js_file);

    EXPECT_SAME_FILE(config->config_file_path(), config_file);
  }
}

TEST_F(test_configuration_loader,
       dot_dot_component_is_resolved_before_finding) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);
    std::string temp_dir = this->make_temporary_directory();
    create_directory(temp_dir + "/dir");
    create_directory(temp_dir + "/dir/subdir");
    std::string config_file_outside_dir = temp_dir + "/" + config_file_name;
    write_file(config_file_outside_dir, u8"{}"sv);
    std::string config_file_inside_subdir =
        temp_dir + "/dir/subdir/" + config_file_name;
    write_file(config_file_inside_subdir, u8"{}"sv);

    // Valid search path order:
    // * $temp_dir/dir/$config_file_name
    // * $temp_dir/$config_file_name
    //
    // Invalid search path order:
    // * $temp_dir/dir/$config_file_name
    //   (i.e. $temp_dir/dir/subdir/../$config_file_name)
    // * $temp_dir/dir/subdir/$config_file_name -- wrong; shouldn't be searched
    // * $temp_dir/dir/$config_file_name
    // * $temp_dir/$config_file_name

    std::string js_file = temp_dir + "/dir/subdir/../hello.js";
    write_file(js_file, u8""sv);
    configuration_loader loader(basic_configuration_filesystem::instance());
    configuration_or_error config = loader.load_for_file(js_file);

    EXPECT_SAME_FILE(config->config_file_path(), config_file_outside_dir);
  }
}

TEST_F(test_configuration_loader, find_config_in_cwd_if_stdin) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  std::string config_file = "quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = nullptr,
      .config_file = nullptr,
      .is_stdin = true,
  });

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, find_config_in_parent_of_cwd_if_stdin) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory(temp_dir + "/dir");
  this->set_current_working_directory(temp_dir + "/dir");
  std::string config_file = "../quick-lint-js.config";
  write_file(config_file, u8"{}"sv);

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = nullptr,
      .config_file = nullptr,
      .is_stdin = true,
  });

  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, file_with_config_file_gets_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";
  write_file(config_file, u8R"({"globals": {"testGlobalVariable": true}})"sv);

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = "hello.js",
      .config_file = config_file.c_str(),
  });

  EXPECT_TRUE(config->globals().find(u8"testGlobalVariable"sv));
  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       files_with_same_config_file_get_same_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";
  write_file(config_file, u8R"({"globals": {"testGlobalVariable": true}})"sv);

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config_one = loader.load_for_file(file_to_lint{
      .path = "one.js",
      .config_file = config_file.c_str(),
  });
  EXPECT_TRUE(config_one.ok());
  configuration_or_error config_two = loader.load_for_file(file_to_lint{
      .path = "two.js",
      .config_file = config_file.c_str(),
  });
  EXPECT_TRUE(config_two.ok());

  EXPECT_EQ(config_one.config, config_two.config)
      << "pointers should be the same";
}

TEST_F(test_configuration_loader,
       files_with_different_config_files_get_different_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file_one = temp_dir + "/config-one.json";
  write_file(config_file_one,
             u8R"({"globals": {"testGlobalVariableOne": true}})"sv);
  std::string config_file_two = temp_dir + "/config-two.json";
  write_file(config_file_two,
             u8R"({"globals": {"testGlobalVariableTwo": true}})"sv);

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config_one = loader.load_for_file(file_to_lint{
      .path = "one.js",
      .config_file = config_file_one.c_str(),
  });
  EXPECT_TRUE(config_one.ok());
  configuration_or_error config_two = loader.load_for_file(file_to_lint{
      .path = "two.js",
      .config_file = config_file_two.c_str(),
  });
  EXPECT_TRUE(config_two.ok());

  EXPECT_NE(config_one.config, config_two.config)
      << "pointers should be different";

  EXPECT_TRUE(config_one->globals().find(u8"testGlobalVariableOne"sv));
  EXPECT_FALSE(config_one->globals().find(u8"testGlobalVariableTwo"sv));
  EXPECT_SAME_FILE(config_one->config_file_path(), config_file_one);

  EXPECT_FALSE(config_two->globals().find(u8"testGlobalVariableOne"sv));
  EXPECT_TRUE(config_two->globals().find(u8"testGlobalVariableTwo"sv));
  EXPECT_SAME_FILE(config_two->config_file_path(), config_file_two);
}

TEST_F(test_configuration_loader, missing_config_file_fails) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";

  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = "hello.js",
      .config_file = config_file.c_str(),
  });

  EXPECT_FALSE(config.ok());
  EXPECT_THAT(config.error,
              HasSubstr(temp_dir + QLJS_PREFERRED_PATH_DIRECTORY_SEPARATOR +
                        "config.json"));
  EXPECT_THAT(config.error,
              AnyOf(HasSubstr("No such file"), HasSubstr("cannot find")));
}

TEST_F(test_configuration_loader,
       found_quick_lint_js_config_is_loaded_only_once) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/" + config_file_name;
    write_file(config_file, u8R"({"globals": {"testGlobalVariable": true}})"sv);

    configuration_loader loader(basic_configuration_filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file(js_file_one, u8""sv);
    configuration_or_error config_one = loader.load_for_file(file_to_lint{
        .path = js_file_one.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_one.ok());
    std::string js_file_two = temp_dir + "/two.js";
    write_file(js_file_two, u8""sv);
    configuration_or_error config_two = loader.load_for_file(file_to_lint{
        .path = js_file_two.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_two.ok());

    EXPECT_EQ(config_one.config, config_two.config)
        << "pointers should be the same";
  }
}

TEST_F(
    test_configuration_loader,
    found_quick_lint_js_config_and_explicit_config_file_is_loaded_only_once) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/" + config_file_name;
    write_file(config_file, u8R"({"globals": {"testGlobalVariable": true}})"sv);

    configuration_loader loader(basic_configuration_filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file(js_file_one, u8""sv);
    configuration_or_error config_one = loader.load_for_file(file_to_lint{
        .path = js_file_one.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_one.ok());
    std::string js_file_two = temp_dir + "/two.js";
    write_file(js_file_two, u8""sv);
    configuration_or_error config_two = loader.load_for_file(file_to_lint{
        .path = js_file_two.c_str(),
        .config_file = config_file.c_str(),
    });
    EXPECT_TRUE(config_two.ok());

    EXPECT_EQ(config_one.config, config_two.config)
        << "pointers should be the same";
  }

  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/" + config_file_name;
    write_file(config_file, u8R"({"globals": {"testGlobalVariable": true}})"sv);

    configuration_loader loader(basic_configuration_filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file(js_file_one, u8""sv);
    configuration_or_error config_one = loader.load_for_file(file_to_lint{
        .path = js_file_one.c_str(),
        .config_file = config_file.c_str(),
    });
    EXPECT_TRUE(config_one.ok());
    std::string js_file_two = temp_dir + "/two.js";
    write_file(js_file_two, u8""sv);
    configuration_or_error config_two = loader.load_for_file(file_to_lint{
        .path = js_file_two.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_two.ok());

    EXPECT_EQ(config_one.config, config_two.config)
        << "pointers should be the same";
  }
}

TEST_F(
    test_configuration_loader,
    found_quick_lint_js_config_and_explicit_dot_quick_lint_js_config_are_loaded_separately) {
  {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/quick-lint-js.config";
    write_file(config_file,
               u8R"({"globals": {"testGlobalVariableNoDot": true}})"sv);
    std::string dot_config_file = temp_dir + "/.quick-lint-js.config";
    write_file(dot_config_file,
               u8R"({"globals": {"testGlobalVariableDot": true}})"sv);

    configuration_loader loader(basic_configuration_filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file(js_file_one, u8""sv);
    configuration_or_error config_one = loader.load_for_file(file_to_lint{
        .path = js_file_one.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_one.ok());
    std::string js_file_two = temp_dir + "/two.js";
    write_file(js_file_two, u8""sv);
    configuration_or_error config_two = loader.load_for_file(file_to_lint{
        .path = js_file_two.c_str(),
        .config_file = dot_config_file.c_str(),
    });
    EXPECT_TRUE(config_two.ok());

    EXPECT_NE(config_one.config, config_two.config)
        << "pointers should be different";
    EXPECT_TRUE(config_one->globals().find(u8"testGlobalVariableNoDot"sv));
    EXPECT_FALSE(config_one->globals().find(u8"testGlobalVariableDot"sv));
    EXPECT_FALSE(config_two->globals().find(u8"testGlobalVariableNoDot"sv));
    EXPECT_TRUE(config_two->globals().find(u8"testGlobalVariableDot"sv));
  }

  {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/quick-lint-js.config";
    write_file(config_file,
               u8R"({"globals": {"testGlobalVariableNoDot": true}})"sv);
    std::string dot_config_file = temp_dir + "/.quick-lint-js.config";
    write_file(dot_config_file,
               u8R"({"globals": {"testGlobalVariableDot": true}})"sv);

    configuration_loader loader(basic_configuration_filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file(js_file_one, u8""sv);
    configuration_or_error config_one = loader.load_for_file(file_to_lint{
        .path = js_file_one.c_str(),
        .config_file = dot_config_file.c_str(),
    });
    EXPECT_TRUE(config_one.ok());
    std::string js_file_two = temp_dir + "/two.js";
    write_file(js_file_two, u8""sv);
    configuration_or_error config_two = loader.load_for_file(file_to_lint{
        .path = js_file_two.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(config_two.ok());

    EXPECT_NE(config_one.config, config_two.config)
        << "pointers should be different";
    EXPECT_FALSE(config_one->globals().find(u8"testGlobalVariableNoDot"sv));
    EXPECT_TRUE(config_one->globals().find(u8"testGlobalVariableDot"sv));
    EXPECT_TRUE(config_two->globals().find(u8"testGlobalVariableNoDot"sv));
    EXPECT_FALSE(config_two->globals().find(u8"testGlobalVariableDot"sv));
  }
}

TEST_F(test_configuration_loader,
       finding_config_succeeds_even_if_file_is_missing) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({})"sv);

  std::string js_file = temp_dir + "/hello.js";
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_TRUE(config.ok());
  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       finding_config_succeeds_even_if_directory_is_missing) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({})"sv);

  std::string js_file = temp_dir + "/dir/hello.js";
  configuration_loader loader(basic_configuration_filesystem::instance());
  configuration_or_error config = loader.load_for_file(js_file);

  EXPECT_TRUE(config.ok());
  EXPECT_SAME_FILE(config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader, config_found_initially_is_unchanged) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file(js_file, u8"");
    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    std::vector<configuration_change> changes = loader.refresh();
    EXPECT_THAT(changes, IsEmpty());
  }
}

TEST_F(test_configuration_loader,
       rewriting_config_completely_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({"globals": {"before": true}})");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  write_file(config_file, u8R"({"globals": {"after": true}})");

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       rewriting_config_partially_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({"globals": {"before": true}})");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  {
    FILE* file = std::fopen(config_file.c_str(), "r+");
    ASSERT_TRUE(file) << std::strerror(errno);
    ASSERT_EQ(std::fseek(file, narrow_cast<long>(strlen(u8R"({"globals": {")")),
                         SEEK_SET),
              0)
        << std::strerror(errno);
    ASSERT_EQ(std::fwrite(u8"after_", 1, 6, file), 6) << std::strerror(errno);
    ASSERT_EQ(std::fclose(file), 0) << std::strerror(errno);
  }

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       rewriting_config_back_to_original_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8R"({"globals": {"a": true}})");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  write_file(config_file, u8R"({"globals": {"b": true}})");
  write_file(config_file, u8R"({"globals": {"a": true}})");

  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(test_configuration_loader,
       renaming_file_over_config_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  create_directory(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file(config_file, u8R"({"globals": {"before": true}})");
  create_directory(project_dir + "/temp");
  std::string new_config_file = project_dir + "/temp/new-config";
  write_file(new_config_file, u8R"({"globals": {"after": true}})");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file(new_config_file, config_file);

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       renaming_file_over_config_with_same_content_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file(config_file, u8"{}");
  create_directory(project_dir + "/temp");
  std::string new_config_file = project_dir + "/temp/new-config";
  write_file(new_config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file(new_config_file, config_file);

  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(test_configuration_loader,
       moving_config_file_away_and_back_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  std::string temp_config_file = project_dir + "/temp.config";
  move_file(config_file, temp_config_file);
  move_file(temp_config_file, config_file);

  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(test_configuration_loader, creating_config_in_same_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file(js_file, u8"");

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
  }
}

TEST_F(test_configuration_loader,
       creating_config_in_same_dir_is_detected_if_file_doesnt_exit) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
    EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
  }
}

TEST_F(test_configuration_loader, creating_config_in_parent_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    create_directory(project_dir + "/dir");
    std::string js_file = project_dir + "/dir/hello.js";
    write_file(js_file, u8"");

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
  }
}

TEST_F(test_configuration_loader,
       creating_shadowing_config_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string secondary_config_file = project_dir + "/.quick-lint-js.config";
  write_file(secondary_config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  std::string primary_config_file = project_dir + "/quick-lint-js.config";
  write_file(primary_config_file, u8"{}");

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), primary_config_file);
}

TEST_F(test_configuration_loader,
       creating_shadowing_config_in_child_dir_is_detected) {
  for (const char* outer_config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    for (const char* inner_config_file_name :
         {"quick-lint-js.config", ".quick-lint-js.config"}) {
      SCOPED_TRACE(outer_config_file_name);
      SCOPED_TRACE(inner_config_file_name);

      std::string project_dir = this->make_temporary_directory();
      create_directory(project_dir + "/dir");
      std::string js_file = project_dir + "/dir/hello.js";
      write_file(js_file, u8"");
      std::string outer_config_file =
          project_dir + "/" + outer_config_file_name;
      write_file(outer_config_file, u8"{}");

      configuration_loader loader(basic_configuration_filesystem::instance());
      loader.load_for_file(js_file);

      std::string inner_config_file =
          project_dir + "/dir/" + inner_config_file_name;
      write_file(inner_config_file, u8"{}");

      std::vector<configuration_change> changes = loader.refresh();
      ASSERT_THAT(changes, ElementsAre(::testing::_));
      EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
      EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                       inner_config_file);
    }
  }
}

TEST_F(test_configuration_loader, deleting_config_in_same_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file(js_file, u8"");
    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    EXPECT_EQ(std::remove(config_file.c_str()), 0)
        << "failed to delete " << config_file << ": " << std::strerror(errno);

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_EQ(changes[0].config->config_file_path(), std::nullopt);
  }
}

TEST_F(test_configuration_loader,
       deleting_shadowing_config_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string primary_config_file = project_dir + "/quick-lint-js.config";
  write_file(primary_config_file, u8"{}");
  std::string secondary_config_file = project_dir + "/.quick-lint-js.config";
  write_file(secondary_config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  EXPECT_EQ(std::remove(primary_config_file.c_str()), 0)
      << "failed to delete " << primary_config_file << ": "
      << std::strerror(errno);

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                   secondary_config_file);
}

TEST_F(test_configuration_loader,
       deleting_shadowing_config_in_child_dir_is_detected) {
  for (const char* outer_config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    for (const char* inner_config_file_name :
         {"quick-lint-js.config", ".quick-lint-js.config"}) {
      SCOPED_TRACE(outer_config_file_name);
      SCOPED_TRACE(inner_config_file_name);

      std::string project_dir = this->make_temporary_directory();
      create_directory(project_dir + "/dir");
      std::string js_file = project_dir + "/dir/hello.js";
      write_file(js_file, u8"");
      std::string outer_config_file =
          project_dir + "/" + outer_config_file_name;
      write_file(outer_config_file, u8"{}");
      std::string inner_config_file =
          project_dir + "/dir/" + inner_config_file_name;
      write_file(inner_config_file, u8"{}");

      configuration_loader loader(basic_configuration_filesystem::instance());
      loader.load_for_file(js_file);

      EXPECT_EQ(std::remove(inner_config_file.c_str()), 0)
          << "failed to delete " << inner_config_file << ": "
          << std::strerror(errno);

      std::vector<configuration_change> changes = loader.refresh();
      ASSERT_THAT(changes, ElementsAre(::testing::_));
      EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
      EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                       outer_config_file);
    }
  }
}

// @@@ needs port
TEST_F(test_configuration_loader,
       moving_config_away_in_same_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file(js_file, u8"");
    std::string config_file = project_dir + "/" + config_file_name;
    write_file(config_file, u8"{}");

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    move_file(config_file, (project_dir + "/moved.config"));

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_EQ(changes[0].config->config_file_path(), std::nullopt);
  }
}

TEST_F(test_configuration_loader,
       moving_shadowing_config_away_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string primary_config_file = project_dir + "/quick-lint-js.config";
  write_file(primary_config_file, u8"{}");
  std::string secondary_config_file = project_dir + "/.quick-lint-js.config";
  write_file(secondary_config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file(primary_config_file, (project_dir + "/moved.config"));

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                   secondary_config_file);
}

TEST_F(test_configuration_loader,
       moving_shadowing_config_away_in_child_dir_is_detected) {
  for (const char* outer_config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    for (const char* inner_config_file_name :
         {"quick-lint-js.config", ".quick-lint-js.config"}) {
      SCOPED_TRACE(outer_config_file_name);
      SCOPED_TRACE(inner_config_file_name);

      std::string project_dir = this->make_temporary_directory();
      create_directory(project_dir + "/dir");
      std::string js_file = project_dir + "/dir/hello.js";
      write_file(js_file, u8"");
      std::string outer_config_file =
          project_dir + "/" + outer_config_file_name;
      write_file(outer_config_file, u8"{}");
      std::string inner_config_file =
          project_dir + "/dir/" + inner_config_file_name;
      write_file(inner_config_file, u8"{}");

      configuration_loader loader(basic_configuration_filesystem::instance());
      loader.load_for_file(js_file);

      move_file(inner_config_file, (project_dir + "/dir/moved.config"));

      std::vector<configuration_change> changes = loader.refresh();
      ASSERT_THAT(changes, ElementsAre(::testing::_));
      EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
      EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                       outer_config_file);
    }
  }
}

TEST_F(test_configuration_loader,
       moving_config_into_same_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file(js_file, u8"");
    std::string temp_config_file = project_dir + "/temp.config";
    write_file(temp_config_file, u8"{}");
    std::string renamed_config_file = project_dir + "/" + config_file_name;

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    move_file(temp_config_file, renamed_config_file);

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                     renamed_config_file);
  }
}

TEST_F(test_configuration_loader,
       moving_config_into_parent_dir_is_detected) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);

    std::string project_dir = this->make_temporary_directory();
    create_directory(project_dir + "/dir");
    std::string js_file = project_dir + "/dir/hello.js";
    write_file(js_file, u8"");
    std::string temp_config_file = project_dir + "/temp.config";
    write_file(temp_config_file, u8"{}");
    std::string renamed_config_file = project_dir + "/" + config_file_name;

    configuration_loader loader(basic_configuration_filesystem::instance());
    loader.load_for_file(js_file);

    move_file(temp_config_file, renamed_config_file);

    std::vector<configuration_change> changes = loader.refresh();
    ASSERT_THAT(changes, ElementsAre(::testing::_));
    EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
    EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                     renamed_config_file);
  }
}

TEST_F(test_configuration_loader,
       moving_shadowing_config_into_child_dir_is_detected) {
  for (const char* outer_config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    for (const char* inner_config_file_name :
         {"quick-lint-js.config", ".quick-lint-js.config"}) {
      SCOPED_TRACE(outer_config_file_name);
      SCOPED_TRACE(inner_config_file_name);

      std::string project_dir = this->make_temporary_directory();
      create_directory(project_dir + "/dir");
      std::string js_file = project_dir + "/dir/hello.js";
      write_file(js_file, u8"");
      std::string outer_config_file =
          project_dir + "/" + outer_config_file_name;
      write_file(outer_config_file, u8"{}");
      std::string temp_config_file = project_dir + "/dir/temp.config";
      write_file(temp_config_file, u8"{}");
      std::string inner_config_file =
          project_dir + "/dir/" + inner_config_file_name;

      configuration_loader loader(basic_configuration_filesystem::instance());
      loader.load_for_file(js_file);

      move_file(temp_config_file, inner_config_file);

      std::vector<configuration_change> changes = loader.refresh();
      ASSERT_THAT(changes, ElementsAre(::testing::_));
      EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
      EXPECT_SAME_FILE(changes[0].config->config_file_path(),
                       inner_config_file);
    }
  }
}

TEST_F(test_configuration_loader,
       moving_shadowing_config_into_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file(js_file, u8"");
  std::string secondary_config_file = project_dir + "/.quick-lint-js.config";
  write_file(secondary_config_file, u8"{}");
  std::string temp_config_file = project_dir + "/temp.config";
  write_file(temp_config_file, u8"{}");
  std::string primary_config_file = project_dir + "/quick-lint-js.config";

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file(temp_config_file, primary_config_file);

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), primary_config_file);
}

TEST_F(test_configuration_loader,
       moving_directory_containing_file_and_config_unlinks_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory(project_dir + "/olddir");
  std::string js_file = project_dir + "/olddir/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/olddir/quick-lint-js.config";
  write_file(config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("olddir"));
  EXPECT_EQ(changes[0].config->config_file_path(), std::nullopt)
      << "config should be removed";
}

TEST_F(test_configuration_loader,
       moving_ancestor_directory_containing_file_and_config_unlinks_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory(project_dir + "/olddir");
  create_directory(project_dir + "/olddir/subdir");
  std::string js_file = project_dir + "/olddir/subdir/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/olddir/subdir/quick-lint-js.config";
  write_file(config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("olddir"));
  EXPECT_EQ(changes[0].config->config_file_path(), std::nullopt)
      << "config should be removed";
}

TEST_F(test_configuration_loader,
       moving_directory_containing_file_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory(project_dir + "/olddir");
  std::string js_file = project_dir + "/olddir/hello.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(test_configuration_loader,
    moving_file_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/oldfile.js";
  write_file(js_file, u8"");
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8"{}");

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  move_file((project_dir + "/oldfile.js"), (project_dir + "/newfile.js"));

  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(test_configuration_loader,
       creating_directory_of_watched_file_and_adding_config_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/dir/test.js";

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  create_directory(project_dir + "/dir");
  std::vector<configuration_change> changes = loader.refresh();
  EXPECT_THAT(changes, IsEmpty())
      << "creating dir should not change associated config file";

  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file(config_file, u8"{}");

  changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_))
      << "adding config should change associated config file";
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("test.js"));
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
    creating_directory_of_watched_file_and_adding_config_is_detected_batched) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/dir/test.js";

  configuration_loader loader(basic_configuration_filesystem::instance());
  loader.load_for_file(js_file);

  create_directory(project_dir + "/dir");
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file(config_file, u8"{}");

  std::vector<configuration_change> changes = loader.refresh();
  ASSERT_THAT(changes, ElementsAre(::testing::_));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("test.js"));
  EXPECT_SAME_FILE(changes[0].config->config_file_path(), config_file);
}

TEST_F(test_configuration_loader,
       creating_config_in_same_dir_as_many_watched_files_is_detected) {
  std::string project_dir = this->make_temporary_directory();

  std::unordered_set<std::string> js_files;
  for (int i = 0; i < 10; ++i) {
    std::string js_file = project_dir + "/hello" + std::to_string(i) + ".js";
    write_file(js_file, u8"");
    auto [_iterator, inserted] = js_files.insert(std::move(js_file));
    ASSERT_TRUE(inserted) << "duplicate js_file: " << js_file;
  }

  configuration_loader loader(basic_configuration_filesystem::instance());
  for (const std::string& js_file : js_files) {
    loader.load_for_file(js_file);
  }

  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file(config_file, u8"{}");

  std::vector<configuration_change> changes = loader.refresh();
  std::unordered_set<std::string> unconfigured_js_files = js_files;
  for (const configuration_change& change : changes) {
    SCOPED_TRACE(*change.watched_path);
    EXPECT_EQ(js_files.count(*change.watched_path), 1)
        << "change should report a watched file";
    EXPECT_EQ(unconfigured_js_files.erase(*change.watched_path), 1)
        << "change should report no duplicate watched files";
    EXPECT_SAME_FILE(change.config->config_file_path(), config_file);
  }
  EXPECT_THAT(unconfigured_js_files, IsEmpty())
      << "all watched files should have a config";
}

TEST(test_configuration_loader_fake,
     file_with_no_config_file_gets_default_config) {
  fake_configuration_filesystem fs;
  fs.create_file(fs.rooted("hello.js"), u8""sv);

  configuration_loader loader(&fs);
  configuration_or_error config = loader.load_for_file(file_to_lint{
      .path = fs.rooted("hello.js").c_str(),
      .config_file = nullptr,
  });
  EXPECT_DEFAULT_CONFIG(*config);
}

TEST(test_configuration_loader_fake,
     find_quick_lint_js_config_in_same_directory) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);
    fake_configuration_filesystem fs;
    fs.create_file(fs.rooted("hello.js"), u8""sv);
    fs.create_file(fs.rooted(config_file_name), u8"{}"sv);

    configuration_loader loader(&fs);
    configuration_or_error config = loader.load_for_file(file_to_lint{
        .path = fs.rooted("hello.js").c_str(),
        .config_file = nullptr,
    });

    EXPECT_EQ(config->config_file_path(), fs.rooted(config_file_name));
  }
}

TEST(test_configuration_loader_fake, find_config_in_parent_directory) {
  for (const char* config_file_name :
       {"quick-lint-js.config", ".quick-lint-js.config"}) {
    SCOPED_TRACE(config_file_name);
    fake_configuration_filesystem fs;
    fs.create_file(fs.rooted("dir/hello.js"), u8""sv);
    fs.create_file(fs.rooted(config_file_name), u8"{}"sv);

    configuration_loader loader(&fs);
    configuration_or_error config = loader.load_for_file(file_to_lint{
        .path = fs.rooted("dir/hello.js").c_str(),
        .config_file = nullptr,
    });

    EXPECT_EQ(config->config_file_path(), fs.rooted(config_file_name));
  }
}

void move_file(const std::string& from, const std::string& to) {
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    int error = errno;
#if defined(_WIN32)
    if (error == EEXIST) {
      BOOL ok = ::ReplaceFileW(
          /*lpReplacedFileName=*/std::filesystem::path(to).wstring().c_str(),
          /*lpReplacementFileName=*/
          std::filesystem::path(from).wstring().c_str(),
          /*lpBackupFileName=*/nullptr,
          /*dwReplacemeFlags=*/0,
          /*lpExclude=*/nullptr,
          /*lpReserved=*/nullptr);
      if (!ok) {
        ADD_FAILURE() << "failed to move " << from << " to " << to << ": "
                      << windows_handle_file::get_last_error_message();
      }
      return;
    }
#endif
    ADD_FAILURE() << "failed to move " << from << " to " << to << ": "
                  << std::strerror(error);
  }
}
}
}

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
