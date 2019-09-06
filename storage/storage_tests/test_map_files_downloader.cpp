#include "storage/storage_tests/test_map_files_downloader.hpp"

#include "std/string.hpp"
#include "std/vector.hpp"

namespace storage
{
void TestMapFilesDownloader::GetServersList(ServersListCallback const & callback)
{
  callback({"http://localhost:34568/unit_tests/"});
}
}  // namespace storage
