fpmng: $(SAPI_FPMNG_PATH)

$(SAPI_FPMNG_PATH): $(PHP_GLOBAL_OBJS) $(PHP_BINARY_OBJS) $(PHP_FASTCGI_OBJS) $(PHP_FPMNG_OBJS)
	$(BUILD_FPMNG)

install-fpmng: $(SAPI_FPMNG_PATH)
	@echo "Installing fpm-ng binary:         $(INSTALL_ROOT)$(sbindir)/"
	@$(mkinstalldirs) $(INSTALL_ROOT)$(sbindir)
	@$(mkinstalldirs) $(INSTALL_ROOT)$(localstatedir)/log
	@$(mkinstalldirs) $(INSTALL_ROOT)$(localstatedir)/run
	@$(LIBTOOL) --mode=install $(INSTALL) -m 0755 $(SAPI_FPMNG_PATH) $(INSTALL_ROOT)$(sbindir)/$(program_prefix)php-fpm-ng$(program_suffix)$(EXEEXT)

	@if test -f "$(INSTALL_ROOT)$(sysconfdir)/php-fpm-ng.conf"; then \
		echo "Installing fpm-ng defconfig:      skipping"; \
	else \
		echo "Installing fpm-ng defconfig:      $(INSTALL_ROOT)$(sysconfdir)/" && \
		$(mkinstalldirs) $(INSTALL_ROOT)$(sysconfdir)/php-fpm-ng.d; \
		$(INSTALL_DATA) sapi/fpmng/php-fpm.conf $(INSTALL_ROOT)$(sysconfdir)/php-fpm-ng.conf.default; \
		$(INSTALL_DATA) sapi/fpmng/www.conf $(INSTALL_ROOT)$(sysconfdir)/php-fpm-ng.d/www.conf.default; \
	fi
