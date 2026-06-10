#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;


int main(int argc, char** argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}