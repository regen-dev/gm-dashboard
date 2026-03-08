CXX       ?= g++
ARCH      ?= native
CXXFLAGS  := -O2 -march=$(ARCH) -pipe -Wall -Wextra -std=c++17 -fPIC
PREFIX   ?= $(HOME)/.local
PLUGDIR   = $(PREFIX)/lib/gm-dashboard
SHAREDIR  = $(PREFIX)/share/gm-dashboard
INCDIR    = $(PREFIX)/include/gm-dashboard
CFGDIR    = $(HOME)/.config/gm-dashboard

# Qt6 path — override with: make QTDIR=/usr/lib/qt6
# System Qt6: sudo apt install qt6-base-dev qt6-webengine-dev qt6-tools-dev
# Qt installer: default ~/Qt/<version>/gcc_64
QTDIR    ?= $(HOME)/Qt/6.10.0/gcc_64

# Qt include/lib paths
QT_CORE_INC = -I$(QTDIR)/include -I$(QTDIR)/include/QtCore
QT_CORE_LIB = -L$(QTDIR)/lib -lQt6Core -Wl,-rpath,$(QTDIR)/lib

QT_VIEWER_INC = $(QT_CORE_INC) \
	-I$(QTDIR)/include/QtGui \
	-I$(QTDIR)/include/QtWidgets \
	-I$(QTDIR)/include/QtWebEngineCore \
	-I$(QTDIR)/include/QtWebEngineWidgets \
	-I$(QTDIR)/include/QtWebChannel \
	-I$(QTDIR)/include/QtPositioning \
	-I$(QTDIR)/include/QtNetwork
QT_VIEWER_LIB = -L$(QTDIR)/lib \
	-lQt6Core -lQt6Gui -lQt6Widgets -lQt6Network \
	-lQt6WebEngineCore -lQt6WebEngineWidgets \
	-lQt6WebChannel -lQt6Positioning \
	-Wl,-rpath,$(QTDIR)/lib

# System libraries
SODIUM_FLAGS  = $(shell pkg-config --cflags --libs libsodium)
SECCOMP_FLAGS = $(shell pkg-config --cflags --libs libseccomp)
CURL_FLAGS    = $(shell pkg-config --cflags --libs libcurl)

PLUGINS = plugins/weather.so plugins/github.so plugins/system.so

all: build sign install
	$(PREFIX)/bin/gm-dashboard

build: gm-dashboard gm-sign gm-config gm-viewer $(PLUGINS)

gm-dashboard: gm-dashboard.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -o $@ $< \
		$(CURL_FLAGS) $(SODIUM_FLAGS) $(SECCOMP_FLAGS) -ldl $(QT_CORE_LIB)

gm-sign: gm-sign.cpp
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -o $@ $< $(SODIUM_FLAGS) $(QT_CORE_LIB)

gm-config: gm-config.cpp
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -o $@ $< $(SODIUM_FLAGS) $(QT_CORE_LIB)

gm-viewer.moc: gm-viewer.cpp
	$(QTDIR)/libexec/moc $< -o $@

gm-viewer: gm-viewer.cpp gm-viewer.moc
	$(CXX) $(CXXFLAGS) $(QT_VIEWER_INC) -o $@ $< $(QT_VIEWER_LIB)

plugins/weather.so: plugins/weather.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -shared -o $@ $< $(CURL_FLAGS) $(QT_CORE_LIB)

plugins/github.so: plugins/github.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -shared -o $@ $< $(QT_CORE_LIB)

plugins/system.so: plugins/system.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) $(QT_CORE_INC) -shared -o $@ $< $(QT_CORE_LIB)

sign: build
	./gm-sign sign plugins/weather.so
	./gm-sign sign plugins/github.so
	./gm-sign sign plugins/system.so

install: build
	install -Dm755 gm-dashboard $(PREFIX)/bin/gm-dashboard
	install -Dm755 gm-sign $(PREFIX)/bin/gm-sign
	install -Dm755 gm-config $(PREFIX)/bin/gm-config
	install -Dm755 gm-viewer $(PREFIX)/bin/gm-viewer
	install -Dm644 gm-plugin.h $(INCDIR)/gm-plugin.h
	install -Dm644 base.html $(SHAREDIR)/base.html
	install -dm755 $(PLUGDIR)
	install -m755 plugins/weather.so $(PLUGDIR)/weather.so
	install -m644 plugins/weather.html $(PLUGDIR)/weather.html
	install -m755 plugins/github.so $(PLUGDIR)/github.so
	install -m644 plugins/github.html $(PLUGDIR)/github.html
	install -m755 plugins/system.so $(PLUGDIR)/system.so
	install -m644 plugins/system.html $(PLUGDIR)/system.html
	@for p in weather github system; do \
		if test -f plugins/$$p.so.sig; then \
			install -m644 plugins/$$p.so.sig $(PLUGDIR)/$$p.so.sig; \
		fi; \
	done
	install -dm700 $(CFGDIR)
	@test -f $(CFGDIR)/config || install -m600 config.example $(CFGDIR)/config
	@test -f $(CFGDIR)/marketplace.pub || \
		(test -f marketplace.pub && install -m644 marketplace.pub $(CFGDIR)/marketplace.pub) || true
	install -Dm644 gm-dashboard.svg $(PREFIX)/share/icons/hicolor/scalable/apps/gm-dashboard.svg
	install -Dm644 gm-dashboard.png $(PREFIX)/share/icons/hicolor/256x256/apps/gm-dashboard.png
	install -Dm644 gm-dashboard.desktop $(PREFIX)/share/applications/gm-dashboard.desktop
	gtk-update-icon-cache -q $(PREFIX)/share/icons/hicolor 2>/dev/null || true
	update-desktop-database -q $(PREFIX)/share/applications 2>/dev/null || true

run: build sign install
	$(PREFIX)/bin/gm-dashboard

# ── Test infrastructure ──────────────────────────────────────────

QT_TEST_INC = $(QT_CORE_INC) -I$(QTDIR)/include/QtTest -I$(QTDIR)/include/QtNetwork
QT_TEST_LIB = -L$(QTDIR)/lib -lQt6Core -lQt6Test -Wl,-rpath,$(QTDIR)/lib

TESTS = tests/test_sign tests/test_weather tests/test_system \
        tests/test_github tests/test_dashboard tests/test_location \
        tests/test_config tests/test_sandbox tests/test_vault

# Pattern rule for moc
tests/%.moc: tests/%.cpp
	$(QTDIR)/libexec/moc $< -o $@

tests/test_sign: tests/test_sign.cpp tests/test_sign.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< $(SODIUM_FLAGS) $(QT_TEST_LIB)

tests/test_weather: tests/test_weather.cpp tests/test_weather.moc plugins/weather.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< $(CURL_FLAGS) $(QT_TEST_LIB)

tests/test_system: tests/test_system.cpp tests/test_system.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< -ldl $(QT_TEST_LIB)

tests/test_github: tests/test_github.cpp tests/test_github.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< -ldl $(QT_TEST_LIB)

tests/test_dashboard: tests/test_dashboard.cpp tests/test_dashboard.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< $(QT_TEST_LIB)

tests/test_location: tests/test_location.cpp tests/test_location.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< \
		-L$(QTDIR)/lib -lQt6Core -lQt6Network -lQt6Test -Wl,-rpath,$(QTDIR)/lib

tests/test_config: tests/test_config.cpp tests/test_config.moc
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< $(QT_TEST_LIB)

tests/stub_nosym.so: tests/stub_nosym.cpp
	$(CXX) $(CXXFLAGS) -shared -o $@ $<

tests/stub_badver.so: tests/stub_badver.cpp gm-plugin.h
	$(CXX) $(CXXFLAGS) -shared -o $@ $<

tests/test_sandbox: tests/test_sandbox.cpp tests/test_sandbox.moc gm-dashboard.cpp gm-plugin.h tests/stub_nosym.so tests/stub_badver.so
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< \
		$(CURL_FLAGS) $(SODIUM_FLAGS) $(SECCOMP_FLAGS) -ldl $(QT_TEST_LIB)

tests/test_vault: tests/test_vault.cpp tests/test_vault.moc gm-config.cpp
	$(CXX) $(CXXFLAGS) $(QT_TEST_INC) -o $@ $< $(SODIUM_FLAGS) $(QT_TEST_LIB)

test: build sign install $(TESTS)
	@failed=0; total=0; \
	for t in $(TESTS); do \
		total=$$((total + 1)); \
		echo ""; \
		echo "══════════════════════════════════════════"; \
		echo "  $$t"; \
		echo "══════════════════════════════════════════"; \
		./$$t -v2 || failed=$$((failed + 1)); \
	done; \
	echo ""; \
	echo "══════════════════════════════════════════"; \
	echo "  RESULTS: $$((total - failed))/$$total passed"; \
	if [ $$failed -gt 0 ]; then \
		echo "  $$failed FAILED"; \
		exit 1; \
	else \
		echo "  ALL PASSED"; \
	fi

clean:
	rm -f gm-dashboard gm-sign gm-config gm-viewer gm-viewer.moc plugins/*.so plugins/*.sig
	rm -f $(TESTS) tests/*.moc tests/stub_*.so

# ── Coverage ─────────────────────────────────────────────────────
COV_CXXFLAGS = -O0 -g --coverage -march=$(ARCH) -std=c++17 -fPIC -Wall -Wextra
COVDIR       = coverage-report

coverage:
	$(MAKE) clean
	find . -name '*.gcda' -o -name '*.gcno' | xargs rm -f 2>/dev/null; true
	$(MAKE) build CXXFLAGS="$(COV_CXXFLAGS)"
	$(MAKE) sign install CXXFLAGS="$(COV_CXXFLAGS)"
	$(MAKE) $(TESTS) CXXFLAGS="$(COV_CXXFLAGS)"
	lcov --zerocounters --directory .
	@failed=0; total=0; \
	for t in $(TESTS); do \
		total=$$((total + 1)); \
		echo ""; \
		echo "── $$t ──"; \
		./$$t -silent || failed=$$((failed + 1)); \
	done; \
	echo ""; \
	echo "Tests: $$((total - failed))/$$total passed"
	lcov --capture --directory . --output-file coverage.info \
		--ignore-errors mismatch,empty,unused 2>/dev/null
	lcov --remove coverage.info '/usr/*' '*/Qt/*' '*/tests/*' '*moc*' \
		'*/gm-viewer.cpp' \
		--output-file coverage.info --ignore-errors unused 2>/dev/null
	genhtml coverage.info --output-directory $(COVDIR) \
		--ignore-errors mismatch 2>/dev/null
	@echo ""
	@echo "══════════════════════════════════════════"
	@lcov --summary coverage.info --ignore-errors empty 2>/dev/null | grep -E 'lines|functions|branches'
	@echo "══════════════════════════════════════════"
	@echo "Report: $(COVDIR)/index.html"

coverage-clean:
	rm -rf $(COVDIR) coverage.info
	find . -name '*.gcda' -o -name '*.gcno' | xargs rm -f 2>/dev/null; true

.PHONY: all build install clean sign run test coverage coverage-clean
