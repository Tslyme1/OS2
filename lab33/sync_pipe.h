int sync_pipe_init();
void sync_pipe_close();
int sync_pipe_wait();
void sync_pipe_notify(int num_really_created_threads);
int get_rfd_spipe();