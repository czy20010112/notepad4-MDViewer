# Makefile for notepad4

PROJ = Notepad4
NAME = $(BINFOLDER)/$(PROJ).exe
OBJDIR = $(BINFOLDER)/obj/$(PROJ)
SRCDIR = ../../src
editlexers_dir = $(SRCDIR)/EditLexers
scintilla_dir = ../../scintilla

INCDIR = \
	-I"../../src" \
	-I"../../src/EditLexers" \
	-I"$(SRCDIR)/webview2/include" \
	-I"$(scintilla_dir)/include"

LDFLAGS += -L"$(BINFOLDER)/obj"

LDLIBS += -limm32

editlexers_src = $(wildcard $(editlexers_dir)/*.cpp)
editlexers_obj = $(patsubst $(editlexers_dir)/%.cpp,$(OBJDIR)/%.obj,$(editlexers_src))

# c_src = $(wildcard $(SRCDIR)/*.c)
# c_obj = $(patsubst $(SRCDIR)/*.c,$(OBJDIR)/%.obj,$(c_src))

md4c_dir = $(SRCDIR)/md4c
md4c_src = $(md4c_dir)/md4c.c $(md4c_dir)/entity.c $(md4c_dir)/md4c-html.c
md4c_obj = $(patsubst $(md4c_dir)/%.c,$(OBJDIR)/%.obj,$(md4c_src))

cpp_src = $(wildcard $(SRCDIR)/*.cpp)
cpp_obj = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.obj,$(cpp_src))

rc_src = $(wildcard $(SRCDIR)/*.rc)
rc_obj = $(patsubst $(SRCDIR)/%.rc,$(OBJDIR)/%.res,$(rc_src))

all: $(NAME) $(BINFOLDER)/WebView2Loader.dll $(BINFOLDER)/MDPreviewAssets

$(NAME): $(editlexers_obj) $(cpp_obj) $(md4c_obj) $(rc_obj)
	$(CXX) $^ $(LDFLAGS) -lscintilla $(LDLIBS) -o $@

$(BINFOLDER)/WebView2Loader.dll: $(SRCDIR)/webview2/x64/WebView2Loader.dll
	cp -f $< $@

$(BINFOLDER)/MDPreviewAssets: $(NAME)
	rm -rf $@
	cp -r $(SRCDIR)/MDPreviewAssets $@

$(editlexers_obj): $(OBJDIR)/%.obj: $(editlexers_dir)/%.cpp
	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $(INCDIR) $< -o $(OBJDIR)/$*.obj

$(md4c_obj): $(OBJDIR)/%.obj: $(md4c_dir)/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $(INCDIR) $< -o $(OBJDIR)/$*.obj

# $(c_obj): $(OBJDIR)/%.obj: $(SRCDIR)/%.c
# 	$(CC) -c $(CPPFLAGS) $(CFLAGS) $(INCDIR) $< -o $(OBJDIR)/$*.obj

$(cpp_obj): $(OBJDIR)/%.obj: $(SRCDIR)/%.cpp
	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $(INCDIR) $< -o $(OBJDIR)/$*.obj

$(rc_obj): $(OBJDIR)/%.res: $(SRCDIR)/%.rc
	$(RC) -c 65001 $(CPPFLAGS) $(RCFLAGS) $< $(OBJDIR)/$*.res

clean:
	@$(RM) -rf $(OBJDIR)
	@$(RM) -f $(NAME)
	@$(RM) -f $(BINFOLDER)/WebView2Loader.dll
	@$(RM) -rf $(BINFOLDER)/MDPreviewAssets
