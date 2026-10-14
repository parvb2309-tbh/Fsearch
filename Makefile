# =============================================================================
# FastSearch - Makefile
# =============================================================================
#
# Build with:   mingw32-make            (release GUI build -> fsearch.exe)
#               mingw32-make console    (console build     -> fsearch-cli.exe)
#               mingw32-make debug      (unoptimised + debug symbols)
#               mingw32-make clean
#
# WHY EACH FLAG MATTERS (do not remove these casually):
#
#   -DUNICODE -D_UNICODE
#       THE most important flags in this file. Windows headers define almost
#       every API twice: CreateWindowExA (ANSI, 1 byte per char) and
#       CreateWindowExW (wide, 2 bytes per char). The plain name
#       "CreateWindowEx" is a MACRO that picks one based on whether UNICODE is
#       defined. Visual Studio defines it for you; MinGW DOES NOT.
#       If you forget these, LVN_GETDISPINFO silently becomes the ANSI variant
#       (-77) while the listview we create sends the wide one (-177). The
#       result: the search results list renders COMPLETELY EMPTY, with no
#       error, no crash, and nothing to search for online. Hours lost.
#
#   -municode
#       Selects the Unicode C runtime startup object, which makes wWinMain /
#       wmain the entry point instead of WinMain / main. Needed on BOTH the
#       compile and link lines. Note this is separate from -DUNICODE above:
#       one picks the entry point, the other picks the API macros.
#
#   -mwindows
#       Marks the .exe as a GUI app so Windows does not open a console window
#       behind it. Side effect: printf() output goes nowhere. That is why the
#       'console' target exists.
#
#   -static -static-libgcc -static-libstdc++
#       Bakes the GCC runtime into the .exe. Without this the program runs
#       fine from an MSYS2 shell (which has the DLLs on PATH) but fails when
#       double-clicked from Explorer with "libstdc++-6.dll was not found".
#
#   -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
#       "Target Windows 10+". Without it the headers hide newer APIs.
#
# LINK ORDER: GNU ld processes left to right and only pulls symbols it already
# knows it needs, so object files MUST come before libraries.
# =============================================================================

CXX      := g++
WINDRES  := windres

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
            -municode -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00

LDFLAGS  := -municode -static -static-libgcc -static-libstdc++

# -lcomctl32  ListView, status bar, SetWindowSubclass
# -lshlwapi   path helper functions
# -lole32     CoInitializeEx, CoTaskMemFree
# -lshell32   ShellExecuteW, SHGetKnownFolderPath, SHOpenFolderAndSelectItems
# -luuid      supplies the FOLDERID_LocalAppData GUID constant. Without it you
#             get "undefined reference to FOLDERID_LocalAppData" at link time.
# -lgdi32     CreateFontIndirectW
LIBS     := -lcomctl32 -lshlwapi -lole32 -lshell32 -luuid -lgdi32 -luser32

SRCDIR   := src
OBJDIR   := build

# Engine sources: everything that is NOT the GUI or the CLI entry point.
ENGINE_SRCS := $(SRCDIR)/util.cpp \
               $(SRCDIR)/index.cpp \
               $(SRCDIR)/scanner.cpp \
               $(SRCDIR)/trie.cpp \
               $(SRCDIR)/search.cpp \
               $(SRCDIR)/persist.cpp

GUI_SRCS := $(SRCDIR)/mainwnd.cpp $(SRCDIR)/main.cpp
CLI_SRCS := $(SRCDIR)/cli.cpp

ENGINE_OBJS := $(ENGINE_SRCS:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
GUI_OBJS    := $(GUI_SRCS:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
CLI_OBJS    := $(CLI_SRCS:$(SRCDIR)/%.cpp=$(OBJDIR)/%-cli.o)

RES_OBJ  := $(OBJDIR)/app.res.o

.PHONY: all console debug clean rebuild

all: fsearch.exe

# ---- GUI build -------------------------------------------------------------
fsearch.exe: $(ENGINE_OBJS) $(GUI_OBJS) $(RES_OBJ)
	$(CXX) $(ENGINE_OBJS) $(GUI_OBJS) $(RES_OBJ) -o $@ $(LDFLAGS) -mwindows $(LIBS)
	@echo "Built $@"

# ---- Console build ---------------------------------------------------------
# Same engine, different entry point, and NO -mwindows so printf works.
console: fsearch-cli.exe

fsearch-cli.exe: $(ENGINE_OBJS) $(CLI_OBJS)
	$(CXX) $(ENGINE_OBJS) $(CLI_OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "Built $@"

# ---- Compile rules ---------------------------------------------------------
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%-cli.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# windres turns the .rc script into a linkable object holding the manifest.
# -O coff is required: that is the object format the Windows linker wants.
$(RES_OBJ): res/app.rc res/app.manifest | $(OBJDIR)
	$(WINDRES) -O coff -i res/app.rc -o $@

# The leading '-' tells make to carry on even if this fails. No output
# redirection here on purpose: "2>NUL" is cmd.exe syntax and "2>/dev/null" is
# POSIX syntax, and mingw32-make may use either shell depending on whether
# sh.exe is on PATH. Writing one of them creates a stray file named NUL when
# the other shell is picked. Since this rule only runs when the directory is
# genuinely missing, there is nothing to suppress anyway.
$(OBJDIR):
	-mkdir "$(OBJDIR)"

debug: CXXFLAGS := $(filter-out -O2,$(CXXFLAGS)) -O0 -g
debug: clean all

clean:
	-rm -rf $(OBJDIR) fsearch.exe fsearch-cli.exe

rebuild: clean all
