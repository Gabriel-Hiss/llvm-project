#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#define RV_NOINLINE __declspec(noinline)
#else
#define RV_NOINLINE __attribute__((noinline))
#endif

template class std::vector<int>;
template class std::vector<char>;
template class std::vector<double>;
template class std::vector<std::string>;
template class std::vector<int *>;
template class std::vector<char *>;
template class std::vector<double *>;
template class std::basic_string<char>;
template class std::basic_string<wchar_t>;
template class std::unique_ptr<int>;
template class std::unique_ptr<int[]>;
template class std::shared_ptr<int>;
template class std::map<int, int>;
template class std::map<std::string, int>;
template class std::unordered_map<int, int>;
template class std::unordered_map<std::string, int>;

template <class T> RV_NOINLINE T *vector_use(std::vector<T> &v, T value) {
  v.reserve(8);
  v.push_back(value);
  v.emplace_back(value);
  v.insert(v.begin(), value);
  auto first = v.begin();
  auto last = v.end();
  if (first != last)
    v.erase(first);
  return v.data() + (v.size() ? v.size() - 1 : 0);
}

template int *vector_use<int>(std::vector<int> &, int);
template char *vector_use<char>(std::vector<char> &, char);
template double *vector_use<double>(std::vector<double> &, double);
template std::string *vector_use<std::string>(std::vector<std::string> &, std::string);
template int **vector_use<int *>(std::vector<int *> &, int *);
template char **vector_use<char *>(std::vector<char *> &, char *);
template double **vector_use<double *>(std::vector<double *> &, double *);

template <class Char>
RV_NOINLINE std::size_t string_use(std::basic_string<Char> &s, const Char *value) {
  s.append(value);
  s.push_back(*value);
  int order = s.compare(value);
  return static_cast<std::size_t>(order) + s.size() + s.data()[0] + s.c_str()[0];
}

template std::size_t string_use<char>(std::basic_string<char> &, const char *);
template std::size_t string_use<wchar_t>(std::basic_string<wchar_t> &, const wchar_t *);

RV_NOINLINE int map_use(std::map<int, int> &m, int key) {
  m.emplace(key, key);
  auto found = m.find(key);
  m[key] = key + 1;
  m.erase(key);
  return found == m.end() ? 0 : found->second;
}

RV_NOINLINE int string_map_use(std::map<std::string, int> &m,
                               const std::string &key) {
  m.emplace(key, 1);
  auto found = m.find(key);
  m[key] = 2;
  return found == m.end() ? 0 : found->second;
}

RV_NOINLINE int unordered_map_use(std::unordered_map<int, int> &m, int key) {
  m.emplace(key, key);
  auto found = m.find(key);
  m[key] = key + 1;
  m.erase(key);
  return found == m.end() ? 0 : found->second;
}

RV_NOINLINE int string_unordered_map_use(
    std::unordered_map<std::string, int> &m, const std::string &key) {
  m.emplace(key, 1);
  auto found = m.find(key);
  m[key] = 2;
  return found == m.end() ? 0 : found->second;
}

RV_NOINLINE int *unique_ptr_use(std::unique_ptr<int> &p) {
  p.reset();
  return p.get();
}

RV_NOINLINE int shared_ptr_use(std::shared_ptr<int> &p) {
  p.reset();
  return p.use_count();
}
