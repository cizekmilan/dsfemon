#ifndef FRONTEND_VIEW_H
#define FRONTEND_VIEW_H

// Render all frontend status rows for one opened frontend fd.
unsigned int render_frontend_status_lines(int frontend_fd, const char *frontend_path, unsigned int line);

#endif
