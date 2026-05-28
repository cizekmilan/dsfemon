#ifndef FRONTEND_VIEW_H
#define FRONTEND_VIEW_H

#include "frontend_status.h"

// Render all frontend status rows from the last collected frontend snapshot.
unsigned int render_frontend_status_lines(const struct frontend_status_snapshot *frontend_status, const char *frontend_path, unsigned int line);

#endif
