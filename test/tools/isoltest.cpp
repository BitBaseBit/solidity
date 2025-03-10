/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libsolutil/AnsiColorized.h>
#include <libsolutil/CommonIO.h>

#include <memory>
#include <test/Common.h>
#include <test/EVMHost.h>
#include <test/InteractiveTests.h>
#include <test/tools/IsolTestOptions.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>

#include "tools/nlohmann.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <regex>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::frontend;
using namespace solidity::frontend::test;
using namespace solidity::util::formatting;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using TestCreator = TestCase::TestCaseCreator;
using TestOptions = solidity::test::IsolTestOptions;
using ContractPath = std::string;
using ContractName = std::string;
using FunctionSig = std::string;
using CallData = std::string;

struct TestStats
{
	int successCount = 0;
	int testCount = 0;
	int skippedCount = 0;
	operator bool() const noexcept { return successCount + skippedCount == testCount; }
	TestStats& operator+=(TestStats const& _other) noexcept
	{
		successCount += _other.successCount;
		testCount += _other.testCount;
		skippedCount += _other.skippedCount;
		return *this;
	}
};

class TestFilter
{
public:
	explicit TestFilter(string _filter): m_filter(std::move(_filter))
	{
		string filter{m_filter};

		boost::replace_all(filter, "/", "\\/");
		boost::replace_all(filter, "*", ".*");

		m_filterExpression = regex{"(" + filter + "(\\.sol|\\.yul))"};
	}

	bool matches(fs::path const& _path, string const& _name) const
	{
		return regex_match(_name, m_filterExpression) && solidity::test::isValidSemanticTestPath(_path);
	}

private:
	string m_filter;
	regex m_filterExpression;
};


class TestTool
{
public:
	TestTool(TestCreator _testCaseCreator, TestOptions const& _options, fs::path _path, string _name)
		: m_testCaseCreator(_testCaseCreator), m_options(_options), m_filter(TestFilter{_options.testFilter}),
		  m_path(std::move(_path)), m_name(std::move(_name)), m_filePath(m_path.string())
	{
	}

	enum class Result
	{
		Success,
		Failure,
		Exception,
		Skipped
	};

	Result process();

	nlohmann::json m_testData;

	static TestStats processPath(
		TestCreator _testCaseCreator, TestOptions const& _options, fs::path const& _basepath, fs::path const& _path);

private:
	enum class Request
	{
		Skip,
		Rerun,
		Quit
	};

	void updateTestCase();
	Request handleResponse(bool _exception);

	TestCreator m_testCaseCreator;
	TestOptions const& m_options;
	TestFilter m_filter;
	fs::path const m_path;
	string const m_name;
	string const m_filePath;

	unique_ptr<TestCase> m_test;

	static bool m_exitRequested;
};

bool TestTool::m_exitRequested = false;

TestTool::Result TestTool::process()
{
	bool formatted{!m_options.noColor};

	try
	{
		if (m_filter.matches(m_path, m_name))
		{
			// (AnsiColorized(cout, formatted, {BOLD}) << m_name << ": ").flush();

			m_test = m_testCaseCreator(TestCase::Config{
				m_path.string(),
				m_options.evmVersion(),
				m_options.vmPaths,
				m_options.enforceViaYul,
				m_options.enforceCompileToEwasm,
				m_options.enforceGasTest,
				m_options.enforceGasTestMinValue});
			if (m_test->shouldRun() && m_test->m_isSemanticTest)
			{
				std::stringstream outputMessages;
				switch (TestCase::TestResult result = m_test->run(outputMessages, "  ", formatted))
				{
				case TestCase::TestResult::Success:
				{
					if (!m_test->m_testData.callData.empty())
						m_testData[m_test->m_testData.contractName][m_test->m_testData.signature]
							= m_test->m_testData.callData;
					// AnsiColorized(cout, formatted, {BOLD, GREEN}) << "OK" << endl;
					return Result::Success;
				}
				default:
					AnsiColorized(std::cout, formatted, {BOLD, RED}) << "FAIL" << endl;

					AnsiColorized(std::cout, formatted, {BOLD, CYAN}) << "  Contract:" << endl;
					m_test->printSource(std::cout, "    ", formatted);
					m_test->printSettings(std::cout, "    ", formatted);

					std::cout << endl << outputMessages.str() << endl;
					return result == TestCase::TestResult::FatalError ? Result::Exception : Result::Failure;
				}
			}
			else
			{
				// AnsiColorized(cout, formatted, {BOLD, YELLOW}) << "NOT RUN" << endl;
				return Result::Skipped;
			}
		}
		else
			return Result::Skipped;
	}
	catch (boost::exception const& _e)
	{
		AnsiColorized(std::cout, formatted, {BOLD, RED})
			<< "Exception during test: " << boost::diagnostic_information(_e) << endl;
		return Result::Exception;
	}
	catch (std::exception const& _e)
	{
		AnsiColorized(std::cout, formatted, {BOLD, RED})
			<< "Exception during test" << (_e.what() ? ": " + string(_e.what()) : ".") << endl;
		return Result::Exception;
	}
	catch (...)
	{
		AnsiColorized(std::cout, formatted, {BOLD, RED}) << "Unknown exception during test." << endl;
		return Result::Exception;
	}
}

void TestTool::updateTestCase()
{
	ofstream file(m_path.string(), ios::trunc);
	m_test->printSource(file);
	m_test->printUpdatedSettings(file);
	file << "// ----" << endl;
	m_test->printUpdatedExpectations(file, "// ");
}

TestTool::Request TestTool::handleResponse(bool _exception)
{
	if (!_exception && m_options.acceptUpdates)
	{
		updateTestCase();
		return Request::Rerun;
	}

	if (_exception)
		std::cout << "(e)dit/(s)kip/(q)uit? ";
	else
		std::cout << "(e)dit/(u)pdate expectations/(s)kip/(q)uit? ";
	std::cout.flush();

	while (true)
	{
		switch (readStandardInputChar())
		{
		case 's':
			std::cout << endl;
			return Request::Skip;
		case 'u':
			if (_exception)
				break;
			else
			{
				std::cout << endl;
				updateTestCase();
				return Request::Rerun;
			}
		case 'e':
			std::cout << endl << endl;
			if (system((m_options.editor + " \"" + m_path.string() + "\"").c_str()))
				cerr << "Error running editor command." << endl << endl;
			return Request::Rerun;
		case 'q':
			std::cout << endl;
			return Request::Quit;
		default:
			break;
		}
	}
}

TestStats TestTool::processPath(
	TestCreator _testCaseCreator, TestOptions const& _options, fs::path const& _basepath, fs::path const& _path)
{
	nlohmann::json testDataJson;
	std::queue<fs::path> paths;
	paths.push(_path);
	int successCount = 0;
	int testCount = 0;
	int skippedCount = 0;

	while (!paths.empty())
	{
		auto currentPath = paths.front();

		fs::path fullpath = _basepath / currentPath;
		if (fs::is_directory(fullpath))
		{
			paths.pop();
			for (auto const& entry: boost::iterator_range<
					 fs::directory_iterator>(fs::directory_iterator(fullpath), fs::directory_iterator()))
				if (fs::is_directory(entry.path()) || TestCase::isTestFilename(entry.path().filename()))
					paths.push(currentPath / entry.path().filename());
		}
		else if (m_exitRequested)
		{
			++testCount;
			paths.pop();
		}
		else
		{
			++testCount;
			TestTool testTool(_testCaseCreator, _options, fullpath, currentPath.generic_path().string());
			auto result = testTool.process();
			if (fullpath.string().find("semanticTests") != std::string::npos)
			{
				testDataJson[fullpath.string()] = testTool.m_testData;
			}

			switch (result)
			{
			case Result::Failure:
			case Result::Exception:
				switch (testTool.handleResponse(result == Result::Exception))
				{
				case Request::Quit:
					paths.pop();
					m_exitRequested = true;
					break;
				case Request::Rerun:
					std::cout << "Re-running test case..." << endl;
					--testCount;
					break;
				case Request::Skip:
					paths.pop();
					++skippedCount;
					break;
				}
				break;
			case Result::Success:
				paths.pop();
				++successCount;
				break;
			case Result::Skipped:
				paths.pop();
				++skippedCount;
				break;
			}
		}
	}
	ofstream testDataFile;
	testDataFile.open("testData.json", std::ios_base::app);
	if (!testDataJson.is_null()) {
		std::cout << testDataJson.dump(2) << std::endl;
		testDataFile << testDataJson.dump(4);
		testDataFile.close();
	}
	return {successCount, testCount, skippedCount};
}

namespace
{
void setupTerminal()
{
#if defined(_WIN32) && defined(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
	// Set output mode to handle virtual terminal (ANSI escape sequences)
	// ignore any error, as this is just a "nice-to-have"
	// only windows needs to be taken care of, as other platforms (Linux/OSX) support them natively.
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut == INVALID_HANDLE_VALUE)
		return;

	DWORD dwMode = 0;
	if (!GetConsoleMode(hOut, &dwMode))
		return;

	dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	if (!SetConsoleMode(hOut, dwMode))
		return;
#endif
}

std::optional<TestStats> runTestSuite(
	TestCreator _testCaseCreator,
	TestOptions const& _options,
	fs::path const& _basePath,
	fs::path const& _subdirectory,
	string const& _name)
{
	fs::path testPath{_basePath / _subdirectory};
	bool formatted{!_options.noColor};
	if (!fs::exists(testPath) || !fs::is_directory(testPath))
	{
		cerr << _name << " tests not found. Use the --testpath argument." << endl;
		return std::nullopt;
	}

	TestStats stats = TestTool::processPath(_testCaseCreator, _options, _basePath, _subdirectory);

	if (stats.skippedCount != stats.testCount)
	{
		std::cout << endl << _name << " Test Summary: ";
		AnsiColorized(std::cout, formatted, {BOLD, stats ? GREEN : RED})
			<< stats.successCount << "/" << stats.testCount;
		std::cout << " tests successful";
		if (stats.skippedCount > 0)
		{
			std::cout << " (";
			AnsiColorized(std::cout, formatted, {BOLD, YELLOW}) << stats.skippedCount;
			std::cout << " tests skipped)";
		}
		std::cout << "." << endl << endl;
	}
	return stats;
}

}

int main(int argc, char const* argv[])
{
	nlohmann::json lol;
	try
	{
		setupTerminal();

		{
			auto options = std::make_unique<solidity::test::IsolTestOptions>();

			if (!options->parse(argc, argv))
				return -1;

			options->validate();
			solidity::test::CommonOptions::setSingleton(std::move(options));
		}

		auto& options = dynamic_cast<solidity::test::IsolTestOptions const&>(solidity::test::CommonOptions::get());

		if (!solidity::test::loadVMs(options))
			return 1;

		if (options.disableSemanticTests)
			std::cout << endl << "--- SKIPPING ALL SEMANTICS TESTS ---" << endl << endl;

		TestStats global_stats{0, 0};
		std::cout << "Running tests..." << endl << endl;

		// Actually run the tests.
		// Interactive tests are added in InteractiveTests.h
		for (auto const& ts: g_interactiveTestsuites)
		{
			if (ts.needsVM && options.disableSemanticTests)
				continue;

			if (ts.smt && options.disableSMT)
				continue;

			auto stats = runTestSuite(ts.testCaseCreator, options, options.testPath / ts.path, ts.subpath, ts.title);
			if (stats)
				global_stats += *stats;
			else
				return 1;
		}

		std::cout << endl << "Summary: ";
		AnsiColorized(std::cout, !options.noColor, {BOLD, global_stats ? GREEN : RED})
			<< global_stats.successCount << "/" << global_stats.testCount;
		std::cout << " tests successful";
		if (global_stats.skippedCount > 0)
		{
			std::cout << " (";
			AnsiColorized(std::cout, !options.noColor, {BOLD, YELLOW}) << global_stats.skippedCount;
			std::cout << " tests skipped)";
		}
		std::cout << "." << endl;

		if (options.disableSemanticTests)
			std::cout << "\nNOTE: Skipped semantics tests.\n" << endl;

		return global_stats ? 0 : 1;
	}
	catch (std::exception const& _exception)
	{
		cerr << _exception.what() << endl;
		return 1;
	}
}
