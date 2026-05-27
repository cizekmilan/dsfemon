#include "command_line.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// Store a formatted parser error and use a simple non-zero return convention.
static int command_line_error(char *buffer, size_t buffer_size, const char *format, ...) {
  va_list args;

  va_start(args, format);
  vsnprintf(buffer, buffer_size, format, args);
  va_end(args);

  return -1;
}

// Parse a non-negative decimal integer without accepting trailing text.
static bool parse_non_negative_int(const char *text, int *value) {
  char *end = NULL;

  if (text == NULL || *text == '\0')
    return false;

  errno = 0;
  long parsed = strtol(text, &end, 10);

  if (errno == ERANGE || end == text || *end != '\0')
    return false;

  if (parsed < 0 || parsed > INT_MAX)
    return false;

  *value = (int)parsed;

  return true;
}

// Validate one adapter number against the statically allocated device table.
static int validate_adapter(int adapter, char *error_buffer, size_t error_buffer_size) {
  if (adapter >= 0 && adapter < DVB_MAX_ADAPTERS)
    return 0;

  return command_line_error(error_buffer, error_buffer_size, "Adapter must be between 0 and %d.", DVB_MAX_ADAPTERS - 1);
}

// Parse a comma-separated adapter selection such as "0" or "0,2,5".
static int parse_adapter_list(const char *text, struct dvb_scan_config *scan_config, char *error_buffer, size_t error_buffer_size) {
  const char *part_start = text;

  if (text == NULL || *text == '\0')
    return command_line_error(error_buffer, error_buffer_size, "Adapter selection must not be empty.");

  scan_config->adapter_filter_enabled = true;
  memset(scan_config->adapter_enabled, 0, sizeof(scan_config->adapter_enabled));

  while (part_start != NULL) {
    const char *comma = strchr(part_start, ',');
    char adapter_text[32];
    int adapter;

    size_t adapter_len = comma == NULL ? strlen(part_start) : (size_t)(comma - part_start);
    if (adapter_len == 0)
      return command_line_error(error_buffer, error_buffer_size, "Adapter selection contains an empty item.");

    if (adapter_len >= sizeof(adapter_text))
      return command_line_error(error_buffer, error_buffer_size, "Adapter number is too long.");

    memcpy(adapter_text, part_start, adapter_len);
    adapter_text[adapter_len] = '\0';

    if (!parse_non_negative_int(adapter_text, &adapter))
      return command_line_error(error_buffer, error_buffer_size, "Adapter selection must contain decimal numbers.");

    if (validate_adapter(adapter, error_buffer, error_buffer_size) != 0)
      return -1;

    scan_config->adapter_enabled[adapter] = true;
    part_start = comma == NULL ? NULL : comma + 1;
  }

  return 0;
}

void init_command_line_options(struct command_line_options *options) {
  options->action = COMMAND_LINE_RUN;
  options->scan_config.min_adapter = DVB_DEFAULT_MIN_ADAPTER;
  options->scan_config.max_adapter = DVB_DEFAULT_MAX_ADAPTER;
  options->scan_config.max_subadapter = DVB_MAX_SUBADAPTERS;
  options->scan_config.adapter_filter_enabled = false;
  memset(options->scan_config.adapter_enabled, 0, sizeof(options->scan_config.adapter_enabled));
}

// Parse CLI options before ncurses starts so help/errors remain ordinary text.
int parse_command_line(int argc, char **argv, struct command_line_options *options, char *error_buffer, size_t error_buffer_size) {
  static const struct option long_options[] = {
      {"adapters", required_argument, NULL, 'a'},
      {"subadapters", required_argument, NULL, 's'},
      {"help", no_argument, NULL, 'h'},
      {"version", no_argument, NULL, 'v'},
      {NULL, 0, NULL, 0}};

  optind = 1;
  opterr = 0;

  int option;

  while ((option = getopt_long(argc, argv, ":a:s:hv", long_options, NULL)) != -1) {
    switch (option) {
      case 'a':
        if (parse_adapter_list(optarg, &options->scan_config, error_buffer, error_buffer_size) != 0)
          return -1;

        break;

      case 's': {
        int subadapters;

        if (!parse_non_negative_int(optarg, &subadapters))
          return command_line_error(error_buffer, error_buffer_size, "Subadapter count must be a decimal number.");

        if (subadapters < 1 || subadapters > DVB_MAX_SUBADAPTERS)
          return command_line_error(error_buffer, error_buffer_size, "Subadapter count must be between 1 and %d.", DVB_MAX_SUBADAPTERS);

        options->scan_config.max_subadapter = subadapters;
        break;
      }

      case 'h':
        options->action = COMMAND_LINE_HELP;
        break;

      case 'v':
        options->action = COMMAND_LINE_VERSION;
        break;

      case ':':
        return command_line_error(error_buffer, error_buffer_size, "Option '-%c' requires an argument.", optopt);

      case '?':
      default:
        if (optopt != 0)
          return command_line_error(error_buffer, error_buffer_size, "Unknown option '-%c'.", optopt);

        return command_line_error(error_buffer, error_buffer_size, "Unknown option '%s'.", argv[optind - 1]);
    }
  }

  if (optind < argc)
    return command_line_error(error_buffer, error_buffer_size, "Unexpected positional argument '%s'.", argv[optind]);

  return 0;
}

void print_usage(FILE *stream, const char *program_name) {
  fprintf(stream, "Usage: %s [options]\n", program_name);
  fprintf(stream, "\n");
  fprintf(stream, "Options:\n");
  fprintf(stream, "  -a, --adapters LIST       scan only selected adapters, for example 0 or 0,2,5\n");
  fprintf(stream, "  -s, --subadapters N       scan N frontends per adapter, default %d\n", DVB_MAX_SUBADAPTERS);
  fprintf(stream, "  -h, --help                show this help\n");
  fprintf(stream, "  -v, --version             show program version\n");
}
