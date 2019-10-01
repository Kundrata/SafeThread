#pragma once
#include <windows.h>
#include "dbghelp.h"
#include "stdio.h"


namespace Stackwalk {

	struct StackFrame
	{
		DWORD64 address;
		HMODULE module;
		std::string name;
		std::string sModName;
		unsigned int line;
		std::string file;
	};

	class raii_context {
		CONTEXT * ctx;
		bool owned = true;
	public:
		raii_context() {
			owned = true;
			ctx = new CONTEXT;
		}
		raii_context(CONTEXT* _ctx) {
			ctx = _ctx;
		}
		raii_context& operator=(CONTEXT* _ctx) {
			if (ctx != nullptr && owned) {
				owned = false;
				delete ctx;
			}
			ctx = _ctx;
			return *this;
		}
		CONTEXT* operator()() {
			return ctx;
		}
		operator bool() {
			return nullptr == ctx;
		}
		~raii_context() {
			if (owned)
				delete ctx;
		}
	};

	class StackWalker
	{
		using FrameVect = std::vector<StackFrame>;

		template<typename... Arguments>
		static std::string strf(const char* s, Arguments&&... args)
		{
			std::string str(1, '\0');

			int n = snprintf(&str[0], 0, s, std::forward<Arguments>(args)...);

			n += 1;
			str.resize(n);
			snprintf(&str[0], n, s, std::forward<Arguments>(args)...);
			str.resize(n - 1);
			return str;
		}

		static std::string basename(const std::string& file)
		{
			unsigned int i = file.find_last_of("\\/");
			if (i == std::string::npos)
			{
				return file;
			}
			else
			{
				return file.substr(i + 1);
			}
		}

	public:
		static inline std::unique_ptr<FrameVect> trace(CONTEXT * _pContext = nullptr)
		{
			std::unique_ptr<FrameVect> frames;

#if _WIN64
			DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
			DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
			HANDLE process = GetCurrentProcess();
			HANDLE thread = GetCurrentThread();

			if (SymInitialize(process, NULL, TRUE) == FALSE)
			{
				printf(__FUNCTION__ ": Failed to call SymInitialize.\n");
				return frames;
			}

			SymSetOptions(SYMOPT_LOAD_LINES);

			raii_context  context;
			if (_pContext == nullptr) {
				context()->ContextFlags = CONTEXT_FULL;
				RtlCaptureContext(context());
			}
			else
				context = _pContext;


#if _WIN64
			STACKFRAME frame = {};
			frame.AddrPC.Offset = context.Rip;
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = context.Rbp;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = context.Rsp;
			frame.AddrStack.Mode = AddrModeFlat;
#else
			STACKFRAME frame = {};
			frame.AddrPC.Offset = context()->Eip;
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = context()->Ebp;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = context()->Esp;
			frame.AddrStack.Mode = AddrModeFlat;
#endif


			bool first = true;
			frames = std::make_unique<FrameVect>();

			while (StackWalk(machine, process, thread, &frame, context(), NULL, SymFunctionTableAccess, SymGetModuleBase, NULL))
			{
				StackFrame f = {};
				f.address = frame.AddrPC.Offset;

#if _WIN64
				DWORD64 moduleBase = 0;
#else
				DWORD moduleBase = 0;
#endif

				moduleBase = SymGetModuleBase(process, frame.AddrPC.Offset);
				f.module = (HMODULE)moduleBase;

				char moduelBuff[MAX_PATH];
				if (moduleBase && GetModuleFileNameA((HINSTANCE)moduleBase, moduelBuff, MAX_PATH))
				{
					f.module = (HMODULE)moduleBase;
					f.sModName = basename(moduelBuff);
				}
				else
				{
					f.sModName = "Unknown Module";
				}
#if _WIN64
				DWORD64 offset = 0;
#else
				DWORD offset = 0;
#endif
				char symbolBuffer[sizeof(IMAGEHLP_SYMBOL) + 255];
				PIMAGEHLP_SYMBOL symbol = (PIMAGEHLP_SYMBOL)symbolBuffer;
				symbol->SizeOfStruct = (sizeof IMAGEHLP_SYMBOL) + 255;
				symbol->MaxNameLength = 254;

				if (SymGetSymFromAddr(process, frame.AddrPC.Offset, &offset, symbol))
				{
					f.name = symbol->Name;
				}
				else
				{
					DWORD error = GetLastError();
					printf(__FUNCTION__ ": Failed to resolve address 0x%X: %u\n", frame.AddrPC.Offset, error);
					f.name = "Unknown Function";
				}

				IMAGEHLP_LINE line;
				line.SizeOfStruct = sizeof(IMAGEHLP_LINE);

				DWORD offset_ln = 0;
				if (SymGetLineFromAddr(process, frame.AddrPC.Offset, &offset_ln, &line))
				{
					f.file = line.FileName;
					f.line = line.LineNumber;
				}
				else
				{
					DWORD error = GetLastError();
					printf(__FUNCTION__ ": Failed to resolve line for 0x%X: %u\n", frame.AddrPC.Offset, error);
					f.line = 0;
				}

				//if (!first)
				{
					frames->push_back(f);
				}
				first = false;
			}

			SymCleanup(process);

			return frames;
		}

		template<typename F>
		static bool walk(F& f, CONTEXT* ctx = nullptr)
		{
			auto frames = trace(ctx);
			if (frames) {
				for (auto& frame : *frames) {
					f(frame);
				}
			}
			return (frames ? true : false);
		}

		template<typename F>
		static void passPrettyTrace(F& f, CONTEXT* ctx = nullptr)
		{
			std::string strTrace;
			walk([&](StackFrame& f) {
				std::string sf =
					strf("%s!+0x%llX -- %s, line %u in file %s ---- Abs: {add: 0x%llX, mod: 0x%llX}\n", f.sModName.c_str(), f.address - (DWORD64)f.module, f.name.c_str(), f.line, f.file.c_str(), f.address, (DWORD64)f.module);
				strTrace.append(sf);
			}, ctx);

			f(strTrace);
		}
	};

} // namespace Stackwalk
