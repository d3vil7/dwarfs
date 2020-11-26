/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include <cstring>

#include <unistd.h>

#include <boost/range/irange.hpp>

#include "dwarfs/metadata_v2.h"

#include "dwarfs/gen-cpp2/metadata_layouts.h"
#include "dwarfs/gen-cpp2/metadata_types.h"
#include "dwarfs/gen-cpp2/metadata_types_custom_protocol.h"
#include <thrift/lib/cpp2/frozen/FrozenUtil.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>
#include <thrift/lib/thrift/gen-cpp2/frozen_types_custom_protocol.h>

namespace dwarfs {

namespace {

const uint16_t READ_ONLY_MASK = ~(S_IWUSR | S_IWGRP | S_IWOTH);

}

template <typename LoggerPolicy>
class metadata_v2_ : public metadata_v2::impl {
 public:
  // TODO: pass folly::ByteRange instead of vector (so we can support memory
  // mapping)
  metadata_v2_(logger& lgr, std::vector<uint8_t>&& meta,
               const struct ::stat* /*defaults*/, int inode_offset)
      : data_(std::move(meta))
      , meta_(::apache::thrift::frozen::mapFrozen<thrift::metadata::metadata>(
            data_))
      , root_(meta_.entries()[meta_.entry_index()[0]])
      , inode_offset_(inode_offset)
      , chunk_index_offset_(meta_.chunk_index_offset())
      , log_(lgr) {
    // TODO: defaults?
    log_.debug() << ::apache::thrift::debugString(meta_.thaw());

    ::apache::thrift::frozen::Layout<thrift::metadata::metadata> layout;
    ::apache::thrift::frozen::schema::Schema schema;
    folly::ByteRange range(data_);
    apache::thrift::CompactSerializer::deserialize(range, schema);
    log_.debug() << ::apache::thrift::debugString(schema);
  }

  void dump(std::ostream& os,
            std::function<void(const std::string&, uint32_t)> const& icb)
      const override;

  size_t size() const override { return data_.size(); }

  bool empty() const override { return data_.empty(); }

  void walk(std::function<void(entry_view)> const& func) const override;

  std::optional<entry_view> find(const char* path) const override;
  std::optional<entry_view> find(int inode) const override;
  std::optional<entry_view> find(int inode, const char* name) const override;

  int getattr(entry_view entry, struct ::stat* stbuf) const override;

#if 0
  size_t block_size() const override {
    return static_cast<size_t>(1) << cfg_->block_size_bits;
  }

  unsigned block_size_bits() const override { return cfg_->block_size_bits; }

  int access(entry_view entry, int mode, uid_t uid,
             gid_t gid) const override;
  directory_view opendir(entry_view entry) const override;
  entry_view
  readdir(directory_view d, size_t offset, std::string* name) const override;
  size_t dirsize(directory_view d) const override {
    return d->count + 2; // adds '.' and '..', which we fake in ;-)
  }
  int readlink(entry_view entry, char* buf, size_t size) const override;
  int readlink(entry_view entry, std::string* buf) const override;
  int statvfs(struct ::statvfs* stbuf) const override;
  int open(entry_view entry) const override;

  const chunk_type* get_chunks(int inode, size_t& num) const override;
#endif

 private:
  void dump(std::ostream& os, const std::string& indent, entry_view entry,
            std::function<void(const std::string&, uint32_t)> const& icb) const;
  void dump(std::ostream& os, const std::string& indent, directory_view dir,
            std::function<void(const std::string&, uint32_t)> const& icb) const;

  std::optional<entry_view> find(directory_view d, std::string_view name) const;

  std::string modestring(uint16_t mode) const;

  size_t reg_file_size(entry_view entry) const {
    auto inode = entry.inode() - chunk_index_offset_;
    uint32_t cur = meta_.chunk_index()[inode];
    uint32_t end = meta_.chunk_index()[inode + 1];
    size_t size = 0;
    while (cur < end) {
      size += meta_.chunks()[cur++].size();
    }
    return size;
  }

  size_t link_size(entry_view entry) const { return link_name(entry).size(); }

  size_t file_size(entry_view entry, uint16_t mode) const {
    if (S_ISREG(mode)) {
      return reg_file_size(entry);
    } else if (S_ISLNK(mode)) {
      return link_size(entry);
    } else {
      return 0;
    }
  }

  directory_view getdir(entry_view entry) const {
    return meta_.directories()[entry.inode()];
  }

  void
  walk(entry_view entry, std::function<void(entry_view)> const& func) const;

  std::optional<entry_view> get_entry(int inode) const {
    inode -= inode_offset_;
    std::optional<entry_view> rv;
    if (inode >= 0 && inode < int(meta_.entry_index().size())) {
      rv = meta_.entries()[meta_.entry_index()[inode]];
    }
    return rv;
  }

  uint16_t entry_mode(entry_view entry) const {
    return meta_.modes()[entry.mode()];
  }

  std::string_view entry_name(entry_view entry) const {
    return meta_.names()[entry.name_index()];
  }

  std::string_view link_name(entry_view entry) const {
    return meta_
        .links()[meta_.link_index()[entry.inode()] - meta_.link_index_offset()];
  }

#if 0
  const char* linkptr(entry_view entry) const {
    return as<char>(entry->u.offset + sizeof(uint16_t));
  }
#endif

  std::vector<uint8_t> data_;
  ::apache::thrift::frozen::MappedFrozen<thrift::metadata::metadata> meta_;
  entry_view root_;
  const int inode_offset_;
  const int chunk_index_offset_;
  log_proxy<LoggerPolicy> log_;
};

template <typename LoggerPolicy>
void metadata_v2_<LoggerPolicy>::dump(
    std::ostream& os, const std::string& indent, entry_view entry,
    std::function<void(const std::string&, uint32_t)> const& icb) const {
  auto mode = entry_mode(entry);
  auto inode = entry.inode();

  os << indent << "<inode:" << inode << "> " << modestring(mode);

  if (inode > 0) {
    os << " " << entry_name(entry);
  }

  if (S_ISREG(mode)) {
    uint32_t beg = meta_.chunk_index()[inode - chunk_index_offset_];
    uint32_t end = meta_.chunk_index()[inode - chunk_index_offset_ + 1];
    os << " [" << beg << ", " << end << "]";
    os << " " << file_size(entry, mode) << "\n";
    icb(indent + "  ", inode);
  } else if (S_ISDIR(mode)) {
    dump(os, indent + "  ", meta_.directories()[inode], std::move(icb));
  } else if (S_ISLNK(mode)) {
    os << " -> " << link_name(entry) << "\n";
  } else {
    os << " (unknown type)\n";
  }
}

template <typename LoggerPolicy>
void metadata_v2_<LoggerPolicy>::dump(
    std::ostream& os, const std::string& indent, directory_view dir,
    std::function<void(const std::string&, uint32_t)> const& icb) const {
  auto count = dir.entry_count();
  auto first = dir.first_entry();
  os << indent << "(" << count << ") entries\n";

  for (size_t i = 0; i < count; ++i) {
    dump(os, indent, meta_.entries()[first + i], icb);
  }
}

template <typename LoggerPolicy>
void metadata_v2_<LoggerPolicy>::dump(
    std::ostream& os,
    std::function<void(const std::string&, uint32_t)> const& icb) const {
  dump(os, "", root_, icb);
}

template <typename LoggerPolicy>
std::string metadata_v2_<LoggerPolicy>::modestring(uint16_t mode) const {
  std::ostringstream oss;

  oss << (mode & S_ISUID ? 'U' : '-');
  oss << (mode & S_ISGID ? 'G' : '-');
  oss << (mode & S_ISVTX ? 'S' : '-');
  oss << (S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-');
  oss << (mode & S_IRUSR ? 'r' : '-');
  oss << (mode & S_IWUSR ? 'w' : '-');
  oss << (mode & S_IXUSR ? 'x' : '-');
  oss << (mode & S_IRGRP ? 'r' : '-');
  oss << (mode & S_IWGRP ? 'w' : '-');
  oss << (mode & S_IXGRP ? 'x' : '-');
  oss << (mode & S_IROTH ? 'r' : '-');
  oss << (mode & S_IWOTH ? 'w' : '-');
  oss << (mode & S_IXOTH ? 'x' : '-');

  return oss.str();
}

template <typename LoggerPolicy>
void metadata_v2_<LoggerPolicy>::walk(
    entry_view entry, std::function<void(entry_view)> const& func) const {
  func(entry);
  if (S_ISDIR(entry.mode())) {
    auto dir = getdir(entry);
    auto cur = dir.first_entry();
    auto end = cur + dir.entry_count();
    while (cur < end) {
      walk(meta_.entries()[cur++], func);
    }
  }
}

template <typename LoggerPolicy>
void metadata_v2_<LoggerPolicy>::walk(
    std::function<void(entry_view)> const& func) const {
  walk(root_, func);
}

template <typename LoggerPolicy>
std::optional<entry_view>
metadata_v2_<LoggerPolicy>::find(directory_view dir,
                                 std::string_view name) const {
  auto first = dir.first_entry();
  auto range = boost::irange(first, first + dir.entry_count());

  auto it = std::lower_bound(
      range.begin(), range.end(), name, [&](auto it, std::string_view name) {
        return entry_name(meta_.entries()[it]).compare(name);
      });

  std::optional<entry_view> rv;

  if (it != range.end()) {
    auto cand = meta_.entries()[*it];

    if (entry_name(cand) == name) {
      rv = cand;
    }
  }

  return rv;
}

template <typename LoggerPolicy>
std::optional<entry_view>
metadata_v2_<LoggerPolicy>::find(const char* path) const {
  while (*path and *path == '/') {
    ++path;
  }

  std::optional<entry_view> entry = root_;

  while (*path) {
    const char* next = ::strchr(path, '/');
    size_t clen = next ? next - path : ::strlen(path);

    entry = find(getdir(*entry), std::string_view(path, clen));

    if (!entry) {
      break;
    }

    path = next ? next + 1 : path + clen;
  }

  return entry;
}

template <typename LoggerPolicy>
std::optional<entry_view> metadata_v2_<LoggerPolicy>::find(int inode) const {
  return get_entry(inode);
}

template <typename LoggerPolicy>
std::optional<entry_view>
metadata_v2_<LoggerPolicy>::find(int inode,
                                 const char* name) const { // TODO: string_view?
  auto entry = get_entry(inode);

  if (entry) {
    entry = find(getdir(*entry), std::string_view(name)); // TODO
  }

  return entry;
}

template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::getattr(entry_view entry,
                                        struct ::stat* stbuf) const {
  ::memset(stbuf, 0, sizeof(*stbuf));

  auto mode = entry_mode(entry);

  stbuf->st_mode = mode & READ_ONLY_MASK;
  stbuf->st_size = file_size(entry, mode);
  stbuf->st_ino = entry.inode() + inode_offset_;
  stbuf->st_blocks = (stbuf->st_size + 511) / 512;
  // stbuf->st_uid = getuid(de);    // TODO
  // stbuf->st_gid = getgid(de);
  // stbuf->st_atime = real_de->atime;
  // stbuf->st_mtime = real_de->mtime;
  // stbuf->st_ctime = real_de->ctime;

  return 0;
}

#if 0
template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::access(entry_view entry, int mode, uid_t uid,
                                    gid_t gid) const {
  return dir_reader_->access(entry, mode, uid, gid);
}

template <typename LoggerPolicy>
directory_view metadata_v2_<LoggerPolicy>::opendir(entry_view entry) const {
  if (S_ISDIR(entry->mode)) {
    return getdir(entry);
  }

  return nullptr;
}

template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::open(entry_view entry) const {
  if (S_ISREG(entry->mode)) {
    return entry->inode;
  }

  return -1;
}

template <typename LoggerPolicy>
entry_view
metadata_v2_<LoggerPolicy>::readdir(directory_view d, size_t offset,
                                 std::string* name) const {
  entry_view entry;

  switch (offset) {
  case 0:
    entry = as<dir_entry>(d->self);

    if (name) {
      name->assign(".");
    }
    break;

  case 1:
    entry = as<dir_entry>(d->parent);

    if (name) {
      name->assign("..");
    }
    break;

  default:
    offset -= 2;

    if (offset < d->count) {
      entry = dir_reader_->readdir(d, offset, name);
    } else {
      return nullptr;
    }

    break;
  }

  return entry;
}

template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::readlink(entry_view entry, char* buf,
                                      size_t size) const {
  if (S_ISLNK(entry->mode)) {
    size_t lsize = linksize(entry);

    ::memcpy(buf, linkptr(entry), std::min(lsize, size));

    if (size > lsize) {
      buf[lsize] = '\0';
    }

    return 0;
  }

  return -EINVAL;
}

template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::readlink(entry_view entry,
                                      std::string* buf) const {
  if (S_ISLNK(entry->mode)) {
    size_t lsize = linksize(entry);

    buf->assign(linkptr(entry), lsize);

    return 0;
  }

  return -EINVAL;
}

template <typename LoggerPolicy>
int metadata_v2_<LoggerPolicy>::statvfs(struct ::statvfs* stbuf) const {
  ::memset(stbuf, 0, sizeof(*stbuf));

  stbuf->f_bsize = 1UL << cfg_->block_size_bits;
  stbuf->f_frsize = 1UL;
  stbuf->f_blocks = cfg_->orig_fs_size;
  stbuf->f_files = cfg_->inode_count;
  stbuf->f_flag = ST_RDONLY;
  stbuf->f_namemax = PATH_MAX;

  return 0;
}

template <typename LoggerPolicy>
const chunk_type*
metadata_v2_<LoggerPolicy>::get_chunks(int inode, size_t& num) const {
  inode -= inode_offset_;
  if (inode < static_cast<int>(cfg_->chunk_index_offset) ||
      inode >= static_cast<int>(cfg_->inode_count)) {
    return nullptr;
  }
  uint32_t off = chunk_index_[inode];
  num = (chunk_index_[inode + 1] - off) / sizeof(chunk_type);
  return as<chunk_type>(off);
}

#endif

void metadata_v2::get_stat_defaults(struct ::stat* defaults) {
  ::memset(defaults, 0, sizeof(struct ::stat));
  defaults->st_uid = ::geteuid();
  defaults->st_gid = ::getegid();
  time_t t = ::time(nullptr);
  defaults->st_atime = t;
  defaults->st_mtime = t;
  defaults->st_ctime = t;
}

metadata_v2::metadata_v2(logger& lgr, std::vector<uint8_t>&& data,
                         const struct ::stat* defaults, int inode_offset)
    : impl_(make_unique_logging_object<metadata_v2::impl, metadata_v2_,
                                       logger_policies>(
          lgr, std::move(data), defaults, inode_offset)) {}
} // namespace dwarfs