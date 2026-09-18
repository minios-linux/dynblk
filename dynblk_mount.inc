/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included by dynblk_cli.c: use the same validated attachment implementation. */
struct mount_options {
	char filesystem[64], module[4096], options[4096];
	unsigned int partition, format, cache_mode;
	bool read_only, fake, nomtab, verbose;
};

static int mount_usage(void)
{
	puts("Usage: mount -t dynblk SOURCE TARGET [-o OPTIONS]\n"
	     "Options: format=auto|dynblk|vmdk,cache=MODE,inner-fstype=TYPE,partition=N,module=PATH,ro,rw\n"
	     "Existing volumes only; no create, format, resize or automatic filesystem check.\n"
	     "The device is automatically removed after its last user closes.\n"
	     "-f validates options without loading a module or touching storage.");
	return 0;
}

static int append_mount_option(struct mount_options *o, const char *text)
{
	size_t used = strlen(o->options), length = strlen(text);
	if (!length || used + !!used + length >= sizeof(o->options))
		return error_message("empty or oversized mount option");
	if (used) o->options[used++] = ',';
	memcpy(o->options + used, text, length + 1);
	return 0;
}

static bool simple_type(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	if (!*p) return false;
	for (; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-'))
			return false;
	return true;
}

static int parse_mount_options(struct mount_options *o, const char *text)
{
	char *copy = strdup(text), *save = NULL, *token;
	int ret = 0;
	if (!copy) return system_error("mount options");
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		if (!strncmp(token, "inner-fstype=", 13)) {
			const char *type = token + 13;
			/* mount -t noTYPE means an exclusion list, not a concrete type. */
			if (!simple_type(type) || !strcmp(type, "dynblk") ||
			    !strncmp(type, "no", 2) || strlen(type) >= sizeof(o->filesystem)) {
				ret = error_message("invalid inner-fstype"); break;
			}
			strcpy(o->filesystem, type);
		} else if (!strncmp(token, "partition=", 10)) {
			char *end;
			unsigned long value;
			errno = 0;
			value = strtoul(token + 10, &end, 10);
			if (errno || token[10] < '1' || token[10] > '9' || *end || value > 65535) {
				ret = error_message("partition must be a positive integer <= 65535"); break;
			}
			o->partition = (unsigned int)value;
		} else if (!strncmp(token, "module=", 7)) {
			if (strlen(token + 7) >= sizeof(o->module) || token[7] != '/') {
				ret = error_message("module requires an absolute dynblk.ko path"); break;
			}
			strcpy(o->module, token + 7);
		} else if (!strncmp(token, "format=", 7)) {
            int parsed = parse_format(token + 7);
            if (parsed < 0) { ret = error_message("unsupported storage format"); break; }
            o->format = (unsigned int)parsed;
        } else if (!strncmp(token, "cache=", 6)) {
            int parsed = de_cache_parse(token + 6);
            if (parsed < 0) { ret = error_message("unsupported cache policy"); break; }
            o->cache_mode = (unsigned int)parsed;
        } else if (!strncmp(token, "size=", 5) || !strcmp(token, "create") ||
		           !strcmp(token, "loop") || !strncmp(token, "offset=", 7) ||
		           !strncmp(token, "sizelimit=", 10) || !strcmp(token, "remount") ||
		           !strcmp(token, "bind") || !strcmp(token, "rbind") || !strcmp(token, "move") ||
		           !strcmp(token, "user") || !strcmp(token, "users") ||
		           !strcmp(token, "owner") || !strcmp(token, "group")) {
			ret = error_message("unsupported or unsafe dynblk mount option"); break;
		} else {
			if (!strcmp(token, "ro")) o->read_only = true;
			if (!strcmp(token, "rw")) o->read_only = false;
			if (append_mount_option(o, token)) { ret = 1; break; }
		}
	}
	free(copy);
	return ret;
}

static int abort_mount_attachment(struct held_attachment *held)
{
	struct dynblk_detach request = {
		.version = DYNBLK_ABI_VERSION, .device = held->device, .cookie = held->cookie
	};
	int ret;
	close(held->fd);
	held->fd = -1;
	if (held->autoclear) {
		close(held->control);
		return 0;
	}
	ret = ioctl(held->control, DYNBLK_DETACH, &request);
	if (ret) system_error("mount rollback: device retained, DYNBLK_DETACH");
	close(held->control);
	unload_module_if_new(held->loaded_before);
	return ret ? 1 : 0;
}

static int command_mount(int argc, char **argv)
{
	struct mount_options o = {0};
	struct held_attachment held = { .fd = -1, .control = -1 };
	const char *source = NULL, *target = NULL;
	char device[64], *command[16];
	struct stat st;
	int i, n = 0, ret;
	bool end_options = false;
	for (i = 0; i < argc; i++) {
		const char *arg = argv[i];
		if (!end_options && help_arg(arg)) return mount_usage();
		if (!end_options && !strcmp(arg, "--")) { end_options = true; continue; }
		if (!end_options && (!strcmp(arg, "-o") || !strcmp(arg, "-t"))) {
			if (++i == argc) return error_message("missing mount option value");
			if (!strcmp(arg, "-o")) {
				if (parse_mount_options(&o, argv[i])) return 1;
			} else if (strcmp(argv[i], "dynblk")) return error_message("helper type must be dynblk");
			continue;
		}
		if (!end_options && arg[0] == '-') {
			for (const char *p = arg + 1; *p; p++) {
				switch (*p) {
				case 'f': o.fake = true; break;
				case 'n': o.nomtab = true; break;
				case 'v': o.verbose = true; break;
				case 's': break; /* Unknown options are still reported, not swallowed. */
				case 'r': o.read_only = true; if (append_mount_option(&o, "ro")) return 1; break;
				case 'w': o.read_only = false; if (append_mount_option(&o, "rw")) return 1; break;
				default: return error_message("unsupported mount helper flag");
				}
			}
			continue;
		}
		if (!source) source = arg;
		else if (!target) target = arg;
		else return error_message("mount needs exactly one source and target");
	}
	if (!source || !target || source[0] != '/' || target[0] != '/')
		return error_message("mount needs absolute source and target paths");
	if (o.fake) {
        printf("format=%s cache=%s\n", de_format_name(o.format), de_cache_name(o.cache_mode));
		printf("DRY RUN: attach %s %s; mount %s on %s; arm last-close teardown\n",
		       source, o.read_only ? "read-only" : "read-write",
		       o.partition ? "selected partition" : "whole disk", target);
		return 0;
	}
	if (geteuid() != 0 || getuid() != 0)
		return error_message("mount.dynblk requires root; setuid/user mounts are unsupported");
	if (stat(target, &st) || !S_ISDIR(st.st_mode))
		return error_message("mount target must be an existing directory");
	if (validate_volume_path(source, false) || validate_module(o.module[0] ? o.module : NULL))
		return 1;
	ret = execute_attach(source, o.module[0] ? o.module : NULL, false,
			     0, 0, "none", 0, o.read_only, &held, o.format, o.cache_mode);
	if (ret) return ret;
	if (ioctl(held.fd, DYNBLK_AUTOCLEAR, &held.cookie)) {
		system_error("arm DYNBLK_AUTOCLEAR");
		abort_mount_attachment(&held);
		return 1;
	}
	held.autoclear = true;
	if (o.partition)
		snprintf(device, sizeof(device), "/dev/dynblk%up%u", held.device, o.partition);
	else
		snprintf(device, sizeof(device), "/dev/dynblk%u", held.device);
	for (i = 0; i < 40; i++) {
		if (!lstat(device, &st) && S_ISBLK(st.st_mode)) break;
		usleep(50000);
	}
	if (i == 40) {
		error_message("selected disk or partition did not appear");
		abort_mount_attachment(&held);
		return 1;
	}
	command[n++] = "mount";
	command[n++] = "-i"; /* Do not dispatch another helper recursively. */
	if (o.nomtab) command[n++] = "-n";
	if (o.verbose) command[n++] = "-v";
	if (o.filesystem[0]) { command[n++] = "-t"; command[n++] = o.filesystem; }
	if (o.options[0]) { command[n++] = "-o"; command[n++] = o.options; }
	command[n++] = "--";
	command[n++] = device;
	command[n++] = (char *)target;
	command[n] = NULL;
	ret = run_program(command);
	if (ret) {
		abort_mount_attachment(&held);
		return ret;
	}
	close(held.fd);
	close(held.control);
	return 0;
}
