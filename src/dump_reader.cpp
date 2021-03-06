#include "dump_reader.hpp"
#include "config.h"

#include <cstdio>
#include <limits>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/noncopyable.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#ifdef HAVE_LEVELDB
#include <leveldb/db.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#else /* HAVE_LEVELDB */
#include <boost/filesystem.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/operations.hpp>
#include <boost/thread.hpp>
#include <boost/make_shared.hpp>
#include <fstream>
//#include <fcntl.h>
#endif /* HAVE_LEVELDB */

#include <boost/spirit/include/qi.hpp>
#include <boost/foreach.hpp>
#include <boost/exception/all.hpp>
#include <boost/throw_exception.hpp>
#include <boost/exception/error_info.hpp>
#include <boost/weak_ptr.hpp>

#define BATCH_SIZE (10240)
#define MAX_MERGESORT_BLOCK_SIZE (67108864)

namespace {

namespace qi = boost::spirit::qi;
#ifndef HAVE_LEVELDB
namespace bio = boost::iostreams;
namespace fs = boost::filesystem;
#endif /* !HAVE_LEVELDB */

struct tag_copy_header;
struct tag_leveldb_status;

typedef boost::error_info<tag_copy_header, std::string>    copy_header;
typedef boost::error_info<tag_leveldb_status, std::string> leveldb_status;

struct popen_error : public boost::exception, std::exception {};
struct fread_error : public boost::exception, std::exception {};
struct early_termination_error : public boost::exception, std::exception {};
struct copy_header_parse_error : public boost::exception, std::exception {};
struct leveldb_error : public boost::exception, std::exception {};

typedef boost::shared_ptr<FILE> pipe_ptr;

static void pipe_closer(FILE *fh) {
  if (fh != NULL) {
    if (pclose(fh) == -1) {
      std::cerr << "ERROR while closing popen." << std::endl;
      abort();
    }
  }
}

struct process 
  : public boost::noncopyable {
  explicit process(const std::string &cmd) 
    : m_fh(popen(cmd.c_str(), "r"), &pipe_closer) {
    if (!m_fh) {
      BOOST_THROW_EXCEPTION(popen_error() << boost::errinfo_file_name(cmd));
    }
  }

  ~process() {
  }

  size_t read(char *buf, size_t len) {
    size_t n = fread(buf, 1, len, m_fh.get());
    if (ferror(m_fh.get()) != 0) {
      boost::weak_ptr<FILE> fh(m_fh);
      BOOST_THROW_EXCEPTION(fread_error() << boost::errinfo_file_handle(fh));
    }
    return n;
  }
    
private:
  pipe_ptr m_fh;
};

template <typename T>
struct to_line_filter 
  : public boost::noncopyable {
  to_line_filter(T &source, size_t buffer_size) 
    : m_source(source), 
      m_buffer(buffer_size, '\0'),
      m_buffer_pos(m_buffer.begin()),
      m_buffer_end(m_buffer_pos) {
  }

  ~to_line_filter() {
  }

  size_t read(std::string &line) {
    line.clear();
    std::string::iterator begin_pos = m_buffer_pos;
    char c = '\0';

    do {
      if (m_buffer_pos == m_buffer_end) {
        line.append(begin_pos, m_buffer_pos);
        if (refill() == 0) {
          return 0;
        }
        begin_pos = m_buffer_pos;
      }

      c = *m_buffer_pos;
      if (c != '\n') {
        ++m_buffer_pos;
      }
    } while (c != '\n');

    line.append(begin_pos, m_buffer_pos);
    ++m_buffer_pos;

    return 1;
  }

private:
  size_t refill() {
    size_t bytes = 0;
    while (bytes < m_buffer.size()) {
      size_t len = m_source.read(&m_buffer[bytes], m_buffer.size() - bytes);
      if (len == 0) {
        break;
      }
      bytes += len;
    }
    m_buffer_pos = m_buffer.begin();
    m_buffer_end = m_buffer.begin() + bytes;
    return bytes;
  }

  T &m_source;
  std::string m_buffer;
  std::string::iterator m_buffer_pos, m_buffer_end;
};

// COPY current_nodes (id, latitude, longitude, changeset_id, visible, "timestamp", tile, version) FROM stdin;
template <typename Iterator>
struct copy_line
  : qi::grammar<Iterator, std::vector<std::string>(), qi::space_type> {

  copy_line(const std::string &table_name)
    : copy_line::base_type(root) {
    using qi::char_;
    using qi::alpha;
    using qi::alnum;
    using qi::lexeme;
    using qi::lit;

    root = lit("COPY") >> lit(table_name) >> lit("(") >> (ident % lit(',')) >> lit(") FROM stdin;");
    ident = (alpha >> *(alnum | char_('_'))) | (lit("\"") >> *(char_ - '"' - '\\') >> lit("\""));
  }

  qi::rule<Iterator, std::vector<std::string>(), qi::space_type> root;
  qi::rule<Iterator, std::string(), qi::space_type> ident;
};

template <typename T>
struct filter_copy_contents 
  : public boost::noncopyable {
  explicit filter_copy_contents(T &source, const std::string &table_name) 
    : m_source(source),
      m_in_copy(false),
      m_start_prefix("COPY "),
      m_end_line("\\."),
      m_grammar(table_name),
      m_table_name(table_name) {
  }

  ~filter_copy_contents() {
  }

  std::vector<std::string> init() {
    std::vector<std::string> column_names;
    std::string line;
    size_t got_data = 0;

    do {
      got_data = m_source.read(line);
      
      if (got_data == 0) {
        BOOST_THROW_EXCEPTION(early_termination_error());
      }

      if (line.compare(0, m_start_prefix.size(), m_start_prefix) == 0) {
        std::string::iterator begin = line.begin();
        std::string::iterator end = line.end();
        bool result = qi::phrase_parse(begin, end, m_grammar, qi::space, column_names);
        if (!result) {
          BOOST_THROW_EXCEPTION(copy_header_parse_error() << copy_header(line));
        }
        m_in_copy = true;
        break;
      }
    } while (true);

    if (!m_in_copy) {
      BOOST_THROW_EXCEPTION(early_termination_error());
    }
    if (column_names.empty()) {
      BOOST_THROW_EXCEPTION(early_termination_error());
    }

    return column_names;
  }

  size_t read(std::string &line) {
    size_t got_data = 0;
    do {
      got_data = m_source.read(line);

      if (got_data == 0) {
        break;
      }

      if (m_in_copy && (line.compare(m_end_line) == 0)) {
        m_in_copy = false;
      }
    } while (!m_in_copy);

    return got_data;
  }

private:
  T &m_source;
  bool m_in_copy;
  const std::string m_start_prefix, m_end_line;
  copy_line<std::string::iterator> m_grammar;
  std::string m_table_name;
};

#ifdef HAVE_LEVELDB  
struct db_writer {
  explicit db_writer(const std::string &table_name)
    : m_db(NULL),
      m_batch(),
      m_batch_size(0),
      m_write_options() {

    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;

    // bigger write buffer, as this is a write-heavy process...
    options.write_buffer_size = 128 * 1024 * 1024;

    leveldb::Status status = leveldb::DB::Open(options, table_name, &m_db);
    if (!status.ok()) {
      BOOST_THROW_EXCEPTION(leveldb_error() << leveldb_status(status.ToString()));
    }
  }

  ~db_writer() {
    delete m_db;
  }

  void finish() {
    if (m_batch_size > 0) {
      m_db->Write(m_write_options, &m_batch);
      m_batch.Clear();
      m_batch_size = 0;
    }
    m_db->CompactRange(NULL, NULL);
  }

  void put(const std::string &k, const std::string &v) {
    m_batch.Put(k, v);
    ++m_batch_size;

    if (m_batch_size >= BATCH_SIZE) {
      m_db->Write(m_write_options, &m_batch);
      m_batch.Clear();
      m_batch_size = 0;
    }
  }
  
  leveldb::DB *m_db;
  leveldb::WriteBatch m_batch;
  size_t m_batch_size;
  leveldb::WriteOptions m_write_options;
};

#else /* HAVE_LEVELDB */

typedef std::pair<std::string, std::string> kv_pair_t;

struct block_reader : public boost::noncopyable {
  block_reader(const std::string &subdir, const std::string &prefix, size_t block_counter)
    : m_file_name((boost::format("%1$s/%2$s_%3$08x.data") % subdir % prefix % block_counter).str()),
      m_end(false) {
    if (!fs::exists(m_file_name)) {
      BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("File '%1%' does not exist.") % m_file_name).str()));
    }
    m_file.open(m_file_name.c_str());
    if (!m_file.is_open()) {
      BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("Unable to open '%1%'.") % m_file_name).str()));
    }
    if (!m_file.good()) {
      BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("File '%1%' is open, but not good.") % m_file_name).str()));
    }

    m_stream.push(bio::gzip_decompressor());
    m_stream.push(m_file);

    next();
  }

  ~block_reader() {
    bio::close(m_stream);
    m_file.close();
  }

  bool at_end() { return m_end; }

  const kv_pair_t &value() { return m_current; }

  void next() {
    uint16_t ksz, vsz;
    
    if (bio::read(m_stream, (char *)&ksz, sizeof(uint16_t)) != sizeof(uint16_t)) { m_end = true; return; }
    if (bio::read(m_stream, (char *)&vsz, sizeof(uint16_t)) != sizeof(uint16_t)) { m_end = true; return; }

    m_current.first.resize(ksz);
    if (bio::read(m_stream, &m_current.first[0], ksz) != ksz) { m_end = true; return; }
    m_current.second.resize(vsz);
    if (bio::read(m_stream, &m_current.second[0], vsz) != vsz) { m_end = true; return; }
  }

  const std::string &file_name() const { return m_file_name; }

private:
  std::string m_file_name;
  bool m_end;
  std::ifstream m_file;
  bio::filtering_streambuf<bio::input> m_stream;
  kv_pair_t m_current;
};

struct block_writer : public boost::noncopyable {
  block_writer(const std::string &subdir, const std::string &bit, size_t block_counter) {
    std::string file_name = (boost::format("%1$s/%2$s_%3$08x.data") % subdir % bit % block_counter).str();
    if (fs::exists(file_name)) {
      fs::remove(file_name);
    }
    m_out.open(file_name.c_str());
    if (!m_out.is_open()) {
      BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("Unable to open '%1%'.") % file_name).str()));
    }
    if (!m_out.good()) {
      BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("File '%1%' is open, but not good.") % file_name).str()));
    }

    m_stream.push(bio::gzip_compressor(1));
    m_stream.push(m_out);

    // TODO: future optimisation
    // int fd = (m_out.rdbuf())->fd();
    // int status = posix_fallocate(fd, 0, MAX_MERGESORT_BLOCK_SIZE);
    // if (status != 0) {
    //   BOOST_THROW_EXCEPTION(std::runtime_error((boost::format("posix_fallocate() on '%1%' failed. status=%2%.") % file_name % status).str()));
    // }
  }

  ~block_writer() {
    bio::close(m_stream);
    m_out.close();
  }

  inline void operator()(const kv_pair_t &kv) {
    const std::string &k = kv.first;
    const std::string &v = kv.second;
    uint16_t key_size = uint16_t(k.size()), val_size = uint16_t(v.size());
    bio::write(m_stream, (const char *)(&key_size), sizeof(uint16_t));
    bio::write(m_stream, (const char *)(&val_size), sizeof(uint16_t));
    bio::write(m_stream, k.c_str(), k.size());
    bio::write(m_stream, v.c_str(), v.size());
  }

private:
  std::ofstream m_out;
  bio::filtering_streambuf<bio::output> m_stream;
};

struct compare_first {
  bool operator()(const kv_pair_t &a, const kv_pair_t &b) const {
    const size_t end = std::min(a.first.size(), b.first.size());
    for (size_t i = 0; i < end; ++i) {
      unsigned char ac = (unsigned char)a.first[i];
      unsigned char bc = (unsigned char)b.first[i];
      if (ac < bc) { return true; }
      if (ac > bc) { return false; }
    }
    return end == a.first.size();
  }
};

struct thread_control_block : public boost::noncopyable {
  std::string m_subdir, m_prefix;
  size_t m_block_number;
  std::vector<kv_pair_t> m_strings;
  std::vector<boost::shared_ptr<thread_control_block> > m_waits;
  boost::shared_ptr<boost::thread> m_thread;
  boost::exception_ptr m_error;

  thread_control_block(std::string subdir, std::string prefix, size_t block_number,
                       std::vector<kv_pair_t> &strings,  
                       std::vector<boost::shared_ptr<thread_control_block> > waits = 
                       std::vector<boost::shared_ptr<thread_control_block> >())
    : m_subdir(subdir), m_prefix(prefix), m_block_number(block_number), m_strings(), m_waits(waits),
      m_thread(), m_error() {
    std::swap(m_strings, strings);
    strings.clear();
    m_thread = boost::make_shared<boost::thread>(boost::bind(&thread_control_block::run, boost::ref(*this)));
  }

  std::string file_name() const {
    return (boost::format("%1$s/%2$s_%3$08x.data") % m_subdir % m_prefix % m_block_number).str();
  }

  static void run(thread_control_block &tcb) {
    try {
      if (tcb.m_waits.size() > 0) {
        tcb.run_merge();

      } else {
        tcb.run_write();
      }

    } catch (...) {
      tcb.m_error = boost::current_exception();
    }
  }

  void run_merge() {
    if (m_waits.size() == 1) {
      // wait for only thread to finish
      thread_control_block &tcb2 = *(m_waits[0]);
      tcb2.m_thread->join();
      if (tcb2.m_error) { boost::rethrow_exception(tcb2.m_error); }
      
      // just move it into place.
      std::string part_file_name = tcb2.file_name();
      std::string final_file_name = file_name();
      fs::rename(part_file_name, final_file_name);
      return;
    }
    
    std::list<block_reader*> readers;
    BOOST_FOREACH(boost::shared_ptr<thread_control_block> tcb2, m_waits) {
      tcb2->m_thread->join();
      if (tcb2->m_error) { boost::rethrow_exception(tcb2->m_error); }
      readers.push_back(new block_reader(tcb2->m_subdir, tcb2->m_prefix, tcb2->m_block_number));
    }
    m_waits.clear();
    
    compare_first comp;
    block_writer writer(m_subdir, m_prefix, m_block_number);
    while (!readers.empty()) {
      std::list<block_reader*>::iterator min_itr = readers.begin();
      kv_pair_t min_pair = (*min_itr)->value();
      
      std::list<block_reader*>::iterator itr = readers.begin();
      ++itr;
      while (itr != readers.end()) {
        const kv_pair_t &val = (*itr)->value();
        if (comp(val, min_pair)) {
          min_pair = val;
          min_itr = itr;
        }
        ++itr;
      }
      
      writer(min_pair);
      
      (*min_itr)->next();
      if ((*min_itr)->at_end()) {
        fs::remove((*min_itr)->file_name());
        delete *min_itr;
        readers.erase(min_itr);
      }
    }
  }

  void run_write() {
    block_writer writer(m_subdir, m_prefix, m_block_number);
    compare_first comp;

    std::sort(m_strings.begin(), m_strings.end(), comp);

    BOOST_FOREACH(const kv_pair_t &kv, m_strings) {
      writer(kv);
    }

    m_strings.clear();
  }
};

struct db_writer : public boost::noncopyable {
  explicit db_writer(const std::string &table_name) 
    : m_subdir(table_name),
      m_block_counter(0),
      m_bytes_this_block(0) {
    fs::create_directories(m_subdir);
  }
  
  ~db_writer() {
    BOOST_FOREACH(boost::shared_ptr<thread_control_block> tcb, m_blocks) {
      try {
        tcb->m_thread->join();
      } catch (...) {
        std::cerr << "Caught exception on " << tcb->file_name() << " but already in destructor." << std::endl;
      }
    }
    BOOST_FOREACH(boost::shared_ptr<thread_control_block> tcb, m_blocks2) {
      try {
        tcb->m_thread->join();
      } catch (...) {
        std::cerr << "Caught exception on " << tcb->file_name() << " but already in destructor." << std::endl;
      }
    }
    BOOST_FOREACH(boost::shared_ptr<thread_control_block> tcb, m_blocks3) {
      try {
        tcb->m_thread->join();
      } catch (...) {
        std::cerr << "Caught exception on " << tcb->file_name() << " but already in destructor." << std::endl;
      }
    }
  }
  
  void finish() {
    if (m_strings.size() > 0) {
      flush_block();
    }
    combine_blocks();
  }
  
  void put(const std::string &k, const std::string &v) {
    if (k.size() > size_t(std::numeric_limits<uint16_t>::max())) {
      BOOST_THROW_EXCEPTION(std::runtime_error("Key too large for uint16_t."));
    }
    if (v.size() > size_t(std::numeric_limits<uint16_t>::max())) {
      BOOST_THROW_EXCEPTION(std::runtime_error("Value too large for uint16_t."));
    }
    size_t bytes = k.size() + v.size() + 2 * sizeof(uint16_t);
    if ((m_bytes_this_block + bytes) > MAX_MERGESORT_BLOCK_SIZE) {
      flush_block();
    }
    m_strings.push_back(make_pair(k, v));
    m_bytes_this_block += bytes;
  }

private:
  std::string m_subdir;
  size_t m_block_counter;
  size_t m_bytes_this_block;
  std::vector<kv_pair_t> m_strings;
  std::vector<boost::shared_ptr<thread_control_block> > m_blocks, m_blocks2, m_blocks3;
  
  void flush_block() {
    static const std::string part_1("part"), part_2("part2"), part_3("part3");
    m_blocks.push_back(boost::make_shared<thread_control_block>(m_subdir, part_1, m_block_counter, boost::ref(m_strings)));
    m_strings.clear();

    if (m_blocks.size() >= 16) {
      m_blocks2.push_back(boost::make_shared<thread_control_block>(m_subdir, part_2, m_block_counter, boost::ref(m_strings), m_blocks));
      m_strings.clear();
      m_blocks.clear();

      if (m_blocks2.size() >= 16) {
        m_blocks3.push_back(boost::make_shared<thread_control_block>(m_subdir, part_3, m_block_counter, boost::ref(m_strings), m_blocks2));
        m_strings.clear();
        m_blocks2.clear();
      }
    }
    m_bytes_this_block = 0;
    ++m_block_counter;
  }

  void combine_blocks() {
    if (m_blocks2.size() > 0) {
      m_blocks.insert(m_blocks.end(), m_blocks2.begin(), m_blocks2.end());
      m_blocks2.clear();
    }
    if (m_blocks3.size() > 0) {
      m_blocks.insert(m_blocks.end(), m_blocks3.begin(), m_blocks3.end());
      m_blocks3.clear();
    }
    thread_control_block tcb(m_subdir, "final", 0, m_strings, m_blocks);
    m_strings.clear();
    tcb.m_thread->join();
    if (tcb.m_error) { boost::rethrow_exception(tcb.m_error); }
  }
};
#endif /* HAVE_LEVELDB */

} // anonymous namespace

struct dump_reader::pimpl {
  pimpl(const std::string &cmd, const std::string &table_name)
    : m_proc(cmd),
      m_line_filter(m_proc, 1024 * 1024),
      m_cont_filter(m_line_filter, table_name),
      m_writer(table_name) {

    // get the headers for the COPY data
    m_column_names = m_cont_filter.init();
  }

  ~pimpl() {
  }

  process m_proc;
  to_line_filter<process> m_line_filter;
  filter_copy_contents<to_line_filter<process> > m_cont_filter;

  db_writer m_writer;

  std::vector<std::string> m_column_names;
};

dump_reader::dump_reader(const std::string &table_name,
                         const std::string &dump_file) 
  : m_impl() {
  std::ostringstream cmd;
  cmd << "pg_restore -a -t " << table_name << " " << dump_file;
  m_impl.reset(new pimpl(cmd.str(), table_name));
}

dump_reader::~dump_reader() {
}  

const std::vector<std::string> &dump_reader::column_names() const {
  return m_impl->m_column_names;
}

size_t dump_reader::read(std::string &line) {
  return m_impl->m_cont_filter.read(line);
}

void dump_reader::put(const std::string &k, const std::string &v) {
  m_impl->m_writer.put(k, v);
}

void dump_reader::finish() {
  m_impl->m_writer.finish();
}
