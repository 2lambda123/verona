// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "ast.h"

#include <iostream>
#include <optional>
#include <regex>

namespace langkit
{
  namespace detail
  {
    class Capture
    {
    private:
      std::map<Token, NodeRange> captures;
      std::map<Location, Node> bindings;
      std::map<Token, Location> defaults;
      Node lookup_;

    public:
      Capture() = default;

      Capture(const Capture& that)
      {
        lookup_ = that.lookup_;
      }

      NodeRange& operator[](Token token)
      {
        return captures[token];
      }

      void def(Token token, Location loc)
      {
        defaults[token] = loc;
      }

      Node operator()(Token token)
      {
        return *captures[token].first;
      }

      Node operator()(Binding binding)
      {
        auto range = captures[binding.first];
        Location loc;

        if (range.first == range.second)
          loc = defaults[binding.first];
        else if ((range.first + 1) != range.second)
          throw std::runtime_error("Can only bind to a single node");
        else
          loc = (*range.first)->location;

        bindings[loc] = binding.second;
        return binding.second;
      }

      void operator+=(const Capture& that)
      {
        captures.insert(that.captures.begin(), that.captures.end());
        bindings.insert(that.bindings.begin(), that.bindings.end());
        defaults.insert(that.defaults.begin(), that.defaults.end());
        lookup_ = that.lookup_;
      }

      Node& lookup()
      {
        return lookup_;
      }

      void clear()
      {
        captures.clear();
        bindings.clear();
        defaults.clear();
        lookup_ = {};
      }

      void bind()
      {
        for (auto& [loc, node] : bindings)
          node->scope()->bind(loc, node);
      }
    };

    class PatternDef
    {
    public:
      virtual ~PatternDef() = default;

      virtual bool match(NodeIt& it, NodeIt end, Capture& captures) const
      {
        return false;
      }
    };

    using PatternPtr = std::shared_ptr<PatternDef>;

    class Cap : public PatternDef
    {
    private:
      Token name;
      PatternPtr pattern;

    public:
      Cap(Token name, PatternPtr pattern) : name(name), pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto begin = it;
        auto captures2 = captures;

        if (!pattern->match(it, end, captures2))
          return false;

        captures += captures2;
        captures[name] = {begin, it};
        return true;
      }
    };

    class Anything : public PatternDef
    {
    public:
      Anything() {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if (it == end)
          return false;

        ++it;
        return true;
      }
    };

    class TokenMatch : public PatternDef
    {
    private:
      Token type;

    public:
      TokenMatch(Token type) : type(type) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if ((it == end) || ((*it)->type != type))
          return false;

        ++it;
        return true;
      }
    };

    class SymbolMatch : public PatternDef
    {
    private:
      Token type;
      Token symbol_type;

    public:
      SymbolMatch(Token type, Token symbol_type)
      : type(type), symbol_type(symbol_type)
      {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if ((it == end) || ((*it)->type != type))
          return false;

        auto find = (*it)->find_first((*it)->location);

        if (!find || (find->type != symbol_type))
          return false;

        captures.lookup() = find;
        ++it;
        return true;
      }
    };

    class LookupMatch : public PatternDef
    {
    private:
      Token type;
      Token symbol_type;

    public:
      LookupMatch(Token type, Token symbol_type)
      : type(type), symbol_type(symbol_type)
      {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if ((it == end) || ((*it)->type != type))
          return false;

        auto lookup = captures.lookup();
        if (!lookup)
          return false;

        auto find = lookup->at((*it)->location, symbol_type);
        if (!find)
          return false;

        captures.lookup() = find;
        ++it;
        return true;
      }
    };

    class RegexMatch : public PatternDef
    {
    private:
      Token type;
      std::regex regex;

    public:
      RegexMatch(Token type, const std::string& r) : type(type)
      {
        regex = std::regex("^" + r, std::regex_constants::optimize);
      }

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if ((it == end) || ((*it)->type != type))
          return false;

        auto s = (*it)->location.view();
        if (!std::regex_match(s.begin(), s.end(), regex))
          return false;

        ++it;
        return true;
      }
    };

    class Opt : public PatternDef
    {
    private:
      PatternPtr pattern;

    public:
      Opt(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto captures2 = captures;

        if (pattern->match(it, end, captures2))
          captures += captures2;

        return true;
      }
    };

    class Rep : public PatternDef
    {
    private:
      PatternPtr pattern;

    public:
      Rep(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        while ((it != end) && pattern->match(it, end, captures))
          ;
        return true;
      }
    };

    class Not : public PatternDef
    {
    private:
      PatternPtr pattern;

    public:
      Not(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if (it == end)
          return false;

        auto captures2 = captures;
        auto begin = it;

        if (pattern->match(it, end, captures2))
        {
          it = begin;
          return false;
        }

        it = begin + 1;
        return true;
      }
    };

    class Seq : public PatternDef
    {
    private:
      PatternPtr first;
      PatternPtr second;

    public:
      Seq(PatternPtr first, PatternPtr second) : first(first), second(second) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto captures2 = captures;
        auto begin = it;

        if (!first->match(it, end, captures2))
          return false;

        if (!second->match(it, end, captures2))
        {
          it = begin;
          return false;
        }

        captures += captures2;
        return true;
      }
    };

    class Choice : public PatternDef
    {
    private:
      PatternPtr first;
      PatternPtr second;

    public:
      Choice(PatternPtr first, PatternPtr second) : first(first), second(second)
      {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto captures2 = captures;

        if (first->match(it, end, captures2))
        {
          captures += captures2;
          return true;
        }

        auto captures3 = captures;

        if (second->match(it, end, captures3))
        {
          captures += captures3;
          return true;
        }

        return false;
      }
    };

    class Inside : public PatternDef
    {
    private:
      Token type;

    public:
      Inside(Token type) : type(type) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if (it == end)
          return false;

        auto parent = (*it)->parent->acquire();
        return parent->type == type;
      }
    };

    class First : public PatternDef
    {
    public:
      First() {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        if (it == end)
          return false;

        auto parent = (*it)->parent->acquire();
        return it == parent->children.begin();
      }
    };

    class Last : public PatternDef
    {
    public:
      Last() {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        return it == end;
      }
    };

    class Children : public PatternDef
    {
    private:
      PatternPtr pattern;
      PatternPtr children;

    public:
      Children(PatternPtr pattern, PatternPtr children)
      : pattern(pattern), children(children)
      {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto captures2 = captures;
        auto begin = it;

        if (!pattern->match(it, end, captures2))
          return false;

        auto it2 = (*begin)->children.begin();
        auto end2 = (*begin)->children.end();

        if (!children->match(it2, end2, captures2))
        {
          it = begin;
          return false;
        }

        captures += captures2;
        return true;
      }
    };

    class Pred : public PatternDef
    {
    private:
      PatternPtr pattern;

    public:
      Pred(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto begin = it;
        auto captures2 = captures;
        bool ok = pattern->match(it, end, captures2);
        it = begin;
        return ok;
      }
    };

    class NegPred : public PatternDef
    {
    private:
      PatternPtr pattern;

    public:
      NegPred(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const override
      {
        auto begin = it;
        auto captures2 = captures;
        bool ok = pattern->match(it, end, captures2);
        it = begin;
        return !ok;
      }
    };

    class Pattern
    {
    private:
      PatternPtr pattern;

    public:
      Pattern(PatternPtr pattern) : pattern(pattern) {}

      bool match(NodeIt& it, NodeIt end, Capture& captures) const
      {
        return pattern->match(it, end, captures);
      }

      Pattern operator[](const Token& name) const
      {
        return {std::make_shared<Cap>(name, pattern)};
      }

      Pattern operator~() const
      {
        return {std::make_shared<Opt>(pattern)};
      }

      Pattern operator-() const
      {
        return {std::make_shared<Pred>(pattern)};
      }

      Pattern operator--() const
      {
        return {std::make_shared<NegPred>(pattern)};
      }

      Pattern operator++(int) const
      {
        return {std::make_shared<Rep>(pattern)};
      }

      Pattern operator!() const
      {
        return {std::make_shared<Not>(pattern)};
      }

      Pattern operator*(Pattern rhs) const
      {
        return {std::make_shared<Seq>(pattern, rhs.pattern)};
      }

      Pattern operator/(Pattern rhs) const
      {
        return {std::make_shared<Choice>(pattern, rhs.pattern)};
      }

      Pattern operator<<(Pattern rhs) const
      {
        return {std::make_shared<Children>(pattern, rhs.pattern)};
      }
    };

    using Effect = std::function<Node(Capture&)>;
    using PatternEffect = std::pair<Pattern, Effect>;

    inline PatternEffect operator>>(Pattern pattern, Effect effect)
    {
      return {pattern, effect};
    }

    struct RangeContents
    {
      NodeRange range;
    };

    struct RangeOr
    {
      NodeRange range;
      Node node;
    };
  }

  const auto Any = detail::Pattern(std::make_shared<detail::Anything>());
  const auto Start = detail::Pattern(std::make_shared<detail::First>());
  const auto End = detail::Pattern(std::make_shared<detail::Last>());

  inline detail::Pattern T(Token type)
  {
    return detail::Pattern(std::make_shared<detail::TokenMatch>(type));
  }

  inline detail::Pattern R(Token type, const std::string& r)
  {
    return detail::Pattern(std::make_shared<detail::RegexMatch>(type, r));
  }

  inline detail::Pattern S(Token type, Token symbol)
  {
    return detail::Pattern(std::make_shared<detail::SymbolMatch>(type, symbol));
  }

  inline detail::Pattern L(Token type, Token symbol)
  {
    return detail::Pattern(std::make_shared<detail::LookupMatch>(type, symbol));
  }

  inline detail::Pattern In(Token type)
  {
    return detail::Pattern(std::make_shared<detail::Inside>(type));
  }

  inline detail::RangeContents operator*(NodeRange range)
  {
    return {range};
  }

  inline detail::RangeOr operator|(NodeRange range, Node node)
  {
    return {range, node};
  }

  inline detail::RangeOr operator|(NodeRange range, Token token)
  {
    return {range, NodeDef::create(token)};
  }

  inline Node operator<<(Node node1, Node node2)
  {
    node1->push_back(node2);
    return node1;
  }

  inline Node operator<<(Node node, NodeRange range)
  {
    node->push_back(range);
    return node;
  }

  inline Node operator<<(Node node, detail::RangeContents range_contents)
  {
    for (auto it = range_contents.range.first;
         it != range_contents.range.second;
         ++it)
    {
      node->push_back({(*it)->children.begin(), (*it)->children.end()});
      (*it)->children.clear();
    }

    return node;
  }

  inline Node operator<<(Node node, detail::RangeOr range_or)
  {
    if (range_or.range.first != range_or.range.second)
      node->push_back(range_or.range);
    else
      node->push_back(range_or.node);

    return node;
  }

  inline Node operator<<(Node node, Token type)
  {
    node->push_back(NodeDef::create(type));
    return node;
  }

  inline Node operator<<(Token type, Node node)
  {
    auto node1 = NodeDef::create(type);
    return node1 << node;
  }

  inline Node operator<<(Token type, NodeRange range)
  {
    auto node = NodeDef::create(type);
    return node << range;
  }

  inline Node operator<<(Token type, detail::RangeContents range_contents)
  {
    auto node = NodeDef::create(type);
    return node << range_contents;
  }

  inline Node operator<<(Token type, detail::RangeOr range_or)
  {
    auto node = NodeDef::create(type);
    return node << range_or;
  }

  inline Node operator<<(Token type1, Token type2)
  {
    auto node = NodeDef::create(type1);
    return node << type2;
  }

  enum class dir
  {
    bottomup,
    topdown,
  };

  class Pass
  {
  private:
    std::vector<detail::PatternEffect> rules;
    dir direction = dir::topdown;

  public:
    Pass() {}
    Pass(const std::initializer_list<detail::PatternEffect>& r) : rules(r) {}

    Pass& operator()(dir d)
    {
      direction = d;
      return *this;
    }

    Pass& operator()(const std::initializer_list<detail::PatternEffect>& r)
    {
      rules.insert(rules.end(), r.begin(), r.end());
      return *this;
    }

    std::pair<Node, size_t> run(Node node)
    {
      // Because apply runs over child nodes, the top node is never visited.
      // Use a synthetic top node.
      Node top = Group;
      top->push_back(node);
      size_t changes;

      switch (direction)
      {
        case dir::bottomup:
        {
          changes = bottom_up(top);
          break;
        }

        case dir::topdown:
        {
          changes = top_down(top);
          break;
        }
      }

      return {top->children.front(), changes};
    }

    Node repeat(Node node)
    {
      size_t changes = 0;

      do
      {
        std::tie(node, changes) = run(node);
      } while (changes > 0);

      return node;
    }

  private:
    size_t bottom_up(Node node)
    {
      size_t changes = 0;

      for (auto& child : node->children)
        changes += bottom_up(child);

      changes += apply(node);
      return changes;
    }

    size_t top_down(Node node)
    {
      size_t changes = apply(node);

      for (auto& child : node->children)
        changes += top_down(child);

      return changes;
    }

    size_t apply(Node node)
    {
      detail::Capture captures;
      auto it = node->children.begin();
      size_t changes = 0;

      while (it != node->children.end())
      {
        bool replaced = false;

        for (auto& rule : rules)
        {
          auto start = it;
          captures.clear();

          if (rule.first.match(it, node->children.end(), captures))
          {
            // Replace [start, it) with whatever the rule builds.
            auto replace = rule.second(captures);
            replace->parent = node.get();
            it = node->children.erase(start, it);
            it = node->children.insert(it, replace);
            captures.bind();
            replaced = true;
            changes++;
            break;
          }
        }

        if (!replaced)
          ++it;
      }

      return changes;
    }
  };
}