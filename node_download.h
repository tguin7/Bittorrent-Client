#ifndef CS4390_NODE_DOWNLOAD_H
#define CS4390_NODE_DOWNLOAD_H

//download complete file from a peer, file is moved to shared_dir
int node_download_from_cache(const char *cache_file, const char *shared_dir);
#endif
