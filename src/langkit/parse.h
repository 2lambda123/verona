// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "ast.h"

#include <optional>
#include <path/path.h>
#include <regex>

namespace langkit
{
  class Parse;

  namespace detail
  {
    class Make;

    class Rule
    {
      friend class langkit::Parse;

    private:
      std::regex regex;
      std::function<void(Make&)> effect;

    public:
      Rule(const std::string& r, std::function<void(Make&)> effect)
      : effect(effect)
      {
        regex = std::regex("^" + r, std::regex_constants::optimize);
      }
    };

    class Make
    {
      friend class langkit::Parse;

    private:
      Node top;
      Node node;
      Location location;
      std::cmatch match_;
      std::optional<std::string> mode_;

    public:
      Make(Source& source)
      {
        node = NodeDef::create(File, {source, 0, 0});
        top = node;
      }

      size_t& pos()
      {
        return location.pos;
      }

      size_t& len()
      {
        return location.len;
      }

      std::pair<size_t, size_t> linecol() const
      {
        return location.linecol();
      }

      const std::cmatch& match() const
      {
        return match_;
      }

      void mode(const std::string& next)
      {
        mode_ = next;
      }

      bool in(const Token& type) const
      {
        return node->type() == type;
      }

      bool previous(const Token& type) const
      {
        if (!in(Group))
          return false;

        auto n = node->back();
        return n && (n->type() == type);
      }

      void add(const Token& type)
      {
        if ((type != Group) && !in(Group))
          push(Group);

        auto n = NodeDef::create(type, location);
        node->push_back(n);
      }

      void seq(const Token& type, std::initializer_list<Token> skip = {})
      {
        if (in(Group))
        {
          while (node->parent()->type().in(skip))
            node = node->parent()->shared_from_this();

          auto p = node->parent();

          if (p->type() == type)
          {
            node = p->shared_from_this();
          }
          else
          {
            auto seq = NodeDef::create(type, location);
            auto group = p->pop_back();
            p->push_back(seq);
            seq->push_back(group);
            node = seq;
          }
        }
        else
        {
          invalid();
        }
      }

      void push(const Token& type)
      {
        add(type);
        node = node->back();
      }

      void pop(const Token& type)
      {
        if (in(type))
        {
          extend();
          node = node->parent()->shared_from_this();
        }
        else
        {
          invalid();
        }
      }

      void term(std::initializer_list<Token> end = {})
      {
        try_pop(Group);

        for (auto& t : end)
          try_pop(t);
      }

      void extend()
      {
        node->extend(location);
      }

      void invalid()
      {
        if (node->type() == Invalid)
          extend();
        else
          add(Invalid);
      }

    private:
      bool try_pop(const Token& type)
      {
        if (in(type))
        {
          extend();
          node = node->parent()->shared_from_this();
          return true;
        }

        return false;
      }

      Node done()
      {
        term();

        while (node->parent())
        {
          add(Unclosed);
          term();
          node = node->parent()->shared_from_this();
          term();
        }

        if (node != top)
          throw std::runtime_error("malformed AST");

        return top;
      }
    };
  }

  enum class depth
  {
    file,
    directory,
    subdirectories
  };

  class Parse
  {
  public:
    using PreF = std::function<bool(Parse&, const std::string&)>;
    using PostF = std::function<void(Parse&, const std::string&, Node)>;

  private:
    depth depth_;

    PreF prefile_;
    PreF predir_;
    PostF postfile_;
    PostF postdir_;
    PostF postparse_;
    std::map<const std::string, std::vector<detail::Rule>> rules;

  public:
    Parse(depth depth_) : depth_(depth_) {}

    Parse& operator()(
      const std::string& mode, const std::initializer_list<detail::Rule> r)
    {
      rules[mode].insert(rules[mode].end(), r.begin(), r.end());
      return *this;
    }

    void prefile(PreF f)
    {
      prefile_ = f;
    }

    void predir(PreF f)
    {
      predir_ = f;
    }

    void postfile(PostF f)
    {
      postfile_ = f;
    }

    void postdir(PostF f)
    {
      postdir_ = f;
    }

    void postparse(PostF f)
    {
      postparse_ = f;
    }

    Node parse(std::string& path)
    {
      auto ast = sub_parse(path);
      auto top = NodeDef::create(Top);
      top->push_back(ast);

      if (postparse_)
        postparse_(*this, path, top);

      return top;
    }

    Node sub_parse(std::string& path)
    {
      Node ast;

      switch (path::type(path))
      {
        case path::Type::File:
        {
          path = path::canonical(path);
          ast = parse_file(path);
          break;
        }

        case path::Type::Directory:
        {
          if (depth_ == depth::file)
            return {};

          path = path::to_directory(path::canonical(path));
          ast = parse_directory(path);
          break;
        }

        default:
          return {};
      }

      return ast;
    }

  private:
    Node parse_file(const std::string& filename)
    {
      if (prefile_ && !prefile_(*this, filename))
        return {};

      auto source = SourceDef::load(filename);

      if (!source)
        return {};

      auto make = detail::Make(source);
      auto it = source->view().cbegin();
      auto st = it;
      auto end = source->view().cend();

      // Find the start rules.
      auto find = rules.find("start");
      if (find == rules.end())
        throw std::runtime_error("unknown mode: start");

      size_t pos = 0;

      while (it != end)
      {
        bool matched = false;

        for (auto& rule : find->second)
        {
          matched = std::regex_search(it, end, make.match_, rule.regex);

          if (matched)
          {
            pos += make.match_.position();
            size_t len = make.match_.length();
            make.location = {source, pos, len};
            rule.effect(make);

            pos += len;
            it = st + pos;

            if (make.mode_)
            {
              find = rules.find(*make.mode_);
              if (find == rules.end())
                throw std::runtime_error("unknown mode: " + *make.mode_);

              make.mode_ = std::nullopt;
            }
            break;
          }
        }

        if (!matched)
        {
          make.invalid();
          it++;
          pos++;
        }
      }

      auto ast = make.done();

      if (postfile_ && ast)
        postfile_(*this, filename, ast);

      return ast;
    }

    Node parse_directory(const std::string& dir)
    {
      if (predir_ && !predir_(*this, dir))
        return {};

      Node top = NodeDef::create(Directory, {SourceDef::directory(dir), 0, 0});

      auto files = path::files(dir);

      for (auto& file : files)
      {
        auto filename = path::join(dir, file);
        auto ast = parse_file(filename);

        if (ast)
          top->push_back(ast);
      }

      if (depth_ == depth::subdirectories)
      {
        auto dirs = path::directories(dir);

        for (auto& subdir : dirs)
        {
          auto filename = path::join(dir, subdir);
          auto ast = parse_directory(filename);

          if (ast)
            top->push_back(ast);
        }
      }

      if (top->empty())
        return {};

      if (postdir_ && top)
        postdir_(*this, dir, top);

      return top;
    }
  };

  inline detail::Rule
  operator>>(const std::string& r, std::function<void(detail::Make&)> make)
  {
    return {r, make};
  }
}
