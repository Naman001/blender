#!/usr/bin/env python3
#
# "make test" for all platforms, running automated tests.

import argparse
import os
import shutil
import sys

import make_utils
from make_utils import call

# Parse arguments

def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ctest-command", default="ctest")
    parser.add_argument("--cmake-command", default="cmake")
    parser.add_argument("--svn-command", default="svn")
    parser.add_argument("--git-command", default="git")
    parser.add_argument("build_directory")
    return parser.parse_args()

args = parse_arguments()
git_command = args.git_command
svn_command = args.svn_command
ctest_command = args.ctest_command
cmake_command = args.cmake_command
build_dir = args.build_directory

if shutil.which(ctest_command) is None:
    sys.stderr.write("ctest not found, can't run tests\n")
    sys.exit(1)

# Test if we are building a specific release version.
release_version = make_utils.git_branch_release_version(git_command)
lib_tests_dirpath = os.path.join('..', 'lib', "tests")

if not os.path.exists(lib_tests_dirpath):
    print("Tests files not found, downloading...")

    if shutil.which(svn_command) is None:
        sys.stderr.write("svn not found, can't checkout test files\n")
        sys.exit(1)

    svn_url = make_utils.svn_libraries_base_url(release_version) + "/tests"
    call([svn_command, "checkout", svn_url, lib_tests_dirpath])

    # Run cmake again to detect tests files.
    os.chdir(build_dir)
    call([cmake_command, "."])

# Run tests
os.chdir(build_dir)
call([ctest_command, ".", "--output-on-failure"])