// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#include "ast.h"

#include <tuple>

namespace langkit
{
  namespace wf
  {
    template<size_t N>
    struct Choice
    {
      std::array<Token, N> types;

      bool check(Node node, std::ostream& out) const
      {
        auto ok = false;

        for (auto& type : types)
        {
          if (node->type() == type)
          {
            ok = true;
            break;
          }
        }

        if (!ok)
        {
          out << node->location().origin_linecol() << "unexpected "
              << node->type().str() << ", expected a ";

          auto n = 0;
          for (auto& type : types)
          {
            out << type.str();
            ++n;

            if (n <= (N - 1))
              out << ", ";
            if (n == (N - 1))
              out << " or ";
          }

          out << std::endl << node->location().str() << std::endl;
        }

        return ok;
      }
    };

    struct SequenceBase
    {};

    template<size_t N>
    struct Sequence : SequenceBase
    {
      Choice<N> types;

      template<typename WF>
      bool check(const WF& top, Node node, std::ostream& out) const
      {
        auto ok = true;

        for (auto& child : *node)
          ok = types.check(child, out) && top.check(child, out) && ok;

        return ok;
      }
    };

    struct FieldBase
    {};

    template<size_t N>
    struct Field : FieldBase
    {
      Token name;
      Choice<N> types;
    };

    struct FieldsBase
    {};

    template<typename... Ts>
    struct Fields : FieldsBase
    {
      static_assert(
        std::conjunction_v<std::is_base_of<FieldBase, Ts>...>, "Not a Field");

      std::tuple<Ts...> fields;

      consteval Fields(const std::tuple<Ts...>& fields) : fields(fields) {}

      template<typename WF>
      bool check(const WF& top, Node node, std::ostream& out) const
      {
        return check_field<0>(top, true, node, node->begin(), node->end(), out);
      }

      template<size_t I, typename WF>
      bool check_field(
        const WF& top,
        bool ok,
        Node node,
        NodeIt child,
        NodeIt end,
        std::ostream& out) const
      {
        if (child == end)
        {
          if constexpr (I < sizeof...(Ts))
          {
            // Too few child nodes.
            out << node->location().origin_linecol()
                << "too few child nodes in " << node->type().str() << std::endl
                << node->location().str() << std::endl;
            return false;
          }
          else
          {
            return ok;
          }
        }

        if constexpr (I >= sizeof...(Ts))
        {
          // Too many child nodes.
          out << (*child)->location().origin_linecol()
              << "too many child nodes in " << node->type().str() << std::endl
              << (*child)->location().str() << std::endl;
          return false;
        }
        else
        {
          auto field = std::get<I>(fields);
          ok = field.types.check(*child, out) && top.check(*child, out) && ok;

          return check_field<I + 1>(top, ok, node, ++child, end, out);
        }
      }
    };

    struct ShapeBase
    {};

    template<typename T>
    struct Shape : ShapeBase
    {
      static_assert(
        std::is_base_of_v<FieldsBase, T> || std::is_base_of_v<SequenceBase, T>,
        "Not a Field or Sequence");

      Token type;
      T shape;
    };

    struct WellformedBase
    {};

    template<typename... Ts>
    struct Wellformed : WellformedBase
    {
      static_assert(
        std::conjunction_v<std::is_base_of<ShapeBase, Ts>...>, "Not a Shape");

      std::tuple<Ts...> shapes;

      template<size_t I = 0>
      bool check(Node node, std::ostream& out) const
      {
        if constexpr (sizeof...(Ts) == 0)
        {
          return true;
        }
        else if constexpr (I >= sizeof...(Ts))
        {
          out << node->location().origin_linecol() << "unexpected "
              << node->type().str() << std::endl
              << node->location().str() << std::endl;
          return false;
        }
        else
        {
          auto shape = std::get<I>(shapes);

          if (node->type() == shape.type)
            return shape.shape.check(*this, node, out);

          return check<I + 1>(node, out);
        }
      }

      auto operator()() const
      {
        return
          [this](Node node, std::ostream& out) { return check(node, out); };
      }
    };

    template<typename... Ts>
    inline auto check(const Wellformed<Ts...>& wf, Node node)
    {
      std::stringstream out;
      auto r = wf.check(node, out);
      return std::make_pair(r, out.str());
    }

    template<size_t I = 0, typename... Ts>
    inline constexpr Index index_fields(
      const Fields<Ts...>& fields, const Token& type, const Token& name)
    {
      if constexpr (I < sizeof...(Ts))
      {
        if (std::get<I>(fields.fields).name == name)
          return Index(type, I);

        return index_fields<I + 1>(fields, type, name);
      }

      return {};
    }

    template<size_t I = 0, typename... Ts>
    inline constexpr Index index_wellformed(
      const Wellformed<Ts...>& wf, const Token& type, const Token& name)
    {
      if constexpr (I < sizeof...(Ts))
      {
        auto shape = std::get<I>(wf.shapes);

        if (type == shape.type)
        {
          // If this shape is for the right type, search it.
          if constexpr (std::is_base_of_v<FieldsBase, decltype(shape.shape)>)
            return index_fields<0>(shape.shape, shape.type, name);

          // Unless it's a sequence, which doesn't have fields.
          return {};
        }

        return index_wellformed<I + 1>(wf, type, name);
      }

      return {};
    }

    template<size_t N>
    inline consteval auto to_wf(const Choice<N>& choice);

    namespace ops
    {
      inline consteval auto operator|(const Token& type1, const Token& type2)
      {
        return Choice<2>{type1, type2};
      }

      template<size_t N>
      inline consteval auto
      operator|(const Token& type, const Choice<N>& choice)
      {
        Choice<N + 1> result;
        result.types[0] = type;
        std::copy_n(choice.types.begin(), N, result.types.begin() + 1);
        return result;
      }

      template<size_t N>
      inline consteval auto
      operator|(const Choice<N>& choice, const Token& type)
      {
        Choice<N + 1> result;
        std::copy_n(choice.types.begin(), N, result.types.begin());
        result.types[N] = type;
        return result;
      }

      template<size_t N1, size_t N2>
      inline consteval auto
      operator|(const Choice<N1>& choice1, const Choice<N2>& choice2)
      {
        Choice<N1 + N2> result;
        std::copy_n(choice1.types.begin(), N1, result.types.begin());
        std::copy_n(choice2.types.begin(), N2, result.types.begin() + N1);
        return result;
      }

      inline consteval auto operator++(const Token& type, int)
      {
        return Sequence<1>{{}, Choice<1>{type}};
      }

      template<size_t N>
      inline consteval auto operator++(const Choice<N>& choice, int)
      {
        return Sequence<N>{{}, choice};
      }

      inline consteval auto operator>>=(const Token& name, const Token& type)
      {
        return Field<1>{{}, name, Choice<1>{type}};
      }

      template<size_t N>
      inline consteval auto
      operator>>=(const Token& name, const Choice<N>& choice)
      {
        return Field<N>{{}, name, choice};
      }

      inline consteval auto operator*(const Token& fst, const Token& snd)
      {
        return Fields{std::make_tuple(fst >>= fst, snd >>= snd)};
      }

      template<size_t N>
      inline consteval auto operator*(const Token& fst, const Field<N>& snd)
      {
        return Fields{std::make_tuple(fst >>= fst, snd)};
      }

      template<typename... Ts>
      inline consteval auto
      operator*(const Token& fst, const Fields<Ts...>& snd)
      {
        return Fields{std::tuple_cat(std::make_tuple(fst >>= fst), snd.fields)};
      }

      template<size_t N>
      inline consteval auto operator*(const Field<N>& fst, const Token& snd)
      {
        return Fields{std::make_tuple(fst, snd >>= snd)};
      }

      template<size_t N1, size_t N2>
      inline consteval auto
      operator*(const Field<N1>& fst, const Field<N2>& snd)
      {
        return Fields{std::make_tuple(fst, snd)};
      }

      template<size_t N, typename... Ts>
      inline consteval auto
      operator*(const Field<N>& fst, const Fields<Ts...>& snd)
      {
        return Fields{std::tuple_cat(std::make_tuple(fst), snd.fields)};
      }

      template<typename... Ts>
      inline consteval auto
      operator*(const Fields<Ts...>& fst, const Token& snd)
      {
        return Fields{std::tuple_cat(fst.fields, std::make_tuple(snd >>= snd))};
      }

      template<typename... Ts, size_t N>
      inline consteval auto
      operator*(const Fields<Ts...>& fst, const Field<N>& snd)
      {
        return Fields{std::tuple_cat(fst.fields, std::make_tuple(snd))};
      }

      template<typename... Ts1, typename... Ts2>
      inline consteval auto
      operator*(const Fields<Ts1...>& fst, const Fields<Ts2...>& snd)
      {
        return Fields{std::tuple_cat(fst.fields, snd.fields)};
      }

      template<size_t N>
      inline consteval auto
      operator<<=(const Token& type, const Sequence<N>& seq)
      {
        return Shape<Sequence<N>>{{}, type, seq};
      }

      template<typename... Ts>
      inline consteval auto
      operator<<=(const Token& type, const Fields<Ts...>& fields)
      {
        return Shape<Fields<Ts...>>{{}, type, fields};
      }

      template<size_t N>
      inline consteval auto
      operator<<=(const Token& type, const Field<N>& field)
      {
        return type <<= Fields{std::make_tuple(field)};
      }

      inline consteval auto operator<<=(const Token& type1, const Token& type2)
      {
        return type1 <<= (type2 >>= type2);
      }

      template<typename... Ts1, typename... Ts2>
      inline consteval auto
      operator|(const Wellformed<Ts1...>& wf1, const Wellformed<Ts2...>& wf2)
      {
        return Wellformed{std::tuple_cat(wf1.shapes, wf2.shapes)};
      }

      template<typename... Ts, typename T>
      inline consteval auto
      operator|(const Wellformed<Ts...>& wf, const Shape<T>& shape)
      {
        return Wellformed<Ts..., Shape<T>>{
          {}, std::tuple_cat(wf.shapes, std::make_tuple(shape))};
      }

      template<typename... Ts, size_t N>
      inline consteval auto
      operator|(const Wellformed<Ts...>& wf, const Choice<N>& choice)
      {
        return wf | to_wf(choice);
      }

      template<typename... Ts>
      inline consteval auto
      operator|(const Wellformed<Ts...>& wf, const Token& type)
      {
        return wf | (type <<= Fields<>({}));
      }

      template<typename T, typename... Ts>
      inline consteval auto
      operator|(const Shape<T>& shape, const Wellformed<Ts...>& wf)
      {
        return Wellformed{std::tuple_cat(std::make_tuple(shape), wf.shapes)};
      }

      template<typename T1, typename T2>
      inline consteval auto
      operator|(const Shape<T1>& shape1, const Shape<T2>& shape2)
      {
        return Wellformed<Shape<T1>, Shape<T2>>{
          {}, std::make_tuple(shape1, shape2)};
      }

      template<typename T, size_t N>
      inline consteval auto
      operator|(const Shape<T>& shape, const Choice<N>& choice)
      {
        return shape | to_wf(choice);
      }

      template<typename T>
      inline consteval auto operator|(const Shape<T>& shape, const Token& type)
      {
        return shape | (type <<= type);
      }

      template<size_t N, typename... Ts>
      inline consteval auto
      operator|(const Choice<N>& choice, const Wellformed<Ts...>& wf)
      {
        return to_wf(choice) | wf;
      }

      template<size_t N, typename T>
      inline consteval auto
      operator|(const Choice<N>& choice, const Shape<T>& shape)
      {
        return to_wf(choice) | shape;
      }

      template<typename... Ts>
      inline consteval auto
      operator|(const Token& type, const Wellformed<Ts...>& wf)
      {
        return (type <<= type) | wf;
      }

      template<typename T>
      inline consteval auto operator|(const Token& type, const Shape<T>& shape)
      {
        return (type <<= type) | shape;
      }
    }

    template<typename T>
    inline consteval auto to_wf(const Shape<T>& shape)
    {
      using namespace ops;
      return Wellformed{} | shape;
    }

    template<size_t I, size_t N, typename... Ts>
    inline consteval auto
    to_wf(const Choice<N>& choice, const Wellformed<Ts...>& wf)
    {
      using namespace ops;

      if constexpr (I >= N)
        return wf;
      else
        return to_wf<I + 1>(choice, wf | (choice.types[I] <<= Fields<>({})));
    }

    template<size_t N>
    inline consteval auto to_wf(const Choice<N>& choice)
    {
      return to_wf<0>(choice, Wellformed{});
    }

    inline consteval auto to_wf(const Token& type)
    {
      using namespace ops;
      return Wellformed{} | (type <<= type);
    }
  }

  template<typename... Ts>
  inline consteval auto
  operator/(const wf::Wellformed<Ts...>& wf, const Token& type)
  {
    return std::make_pair(wf, type);
  }

  template<typename... Ts>
  inline consteval Index operator/(
    const std::pair<wf::Wellformed<Ts...>, Token>& pair, const Token& name)
  {
    return wf::index_wellformed(pair.first, pair.second, name);
  }
}
