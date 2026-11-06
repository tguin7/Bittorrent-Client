#ifndef CS4390_NODE_DOWNLOAD_H
#define CS4390_NODE_DOWNLOAD_H
/* Initiate concurrent file download after peer.c verified and saved a .track file.
 * The returned file is placed in shared_dir and the cached .track is deleted
 * after its contents match the advertised MD5 checksum. */
int node_download_from_cache(const char *cache_file, const char *shared_dir);
#endif
