#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include <stddef.h>
#include <stdio.h>

#include "device_discovery.h"

// Requested top-level action after command-line parsing.
enum command_line_action {
  COMMAND_LINE_RUN,
  COMMAND_LINE_HELP,
  COMMAND_LINE_VERSION
};

// Runtime options derived from command-line arguments.
struct command_line_options {
  enum command_line_action action;
  struct dvb_scan_config scan_config;
};

void init_command_line_options(struct command_line_options *options);
int parse_command_line(int argc, char **argv, struct command_line_options *options, char *error_buffer, size_t error_buffer_size);
void print_usage(FILE *stream, const char *program_name);

#endif
