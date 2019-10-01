#include <type_traits>
#include <string>



template<typename C>
struct extract_char_type : std::false_type { typedef C char_type; };

template<typename C>
struct extract_char_type<std::basic_string<C>> : std::true_type { typedef C char_type; };

template<typename C>
struct extract_char_type<std::basic_string<C>&> : std::true_type { typedef C char_type; };

template<typename C>
struct extract_char_type<const std::basic_string<C>> : std::true_type { typedef C char_type; };

template<typename C>
struct extract_char_type<const std::basic_string<C>&> : std::true_type { typedef C char_type; };



template<typename T,
	typename Strip = std::decay<T>::type,
	typename C = std::conditional<extract_char_type<Strip>::value,
	extract_char_type<Strip>::char_type, Strip>::type>
	struct is_string {
	typedef typename std::conditional<
		std::is_same<typename std::decay<typename std::remove_pointer<typename std::decay<C>::type>::type>::type, char>::value ||
		std::is_same<typename std::decay<typename std::remove_pointer<typename std::decay<C>::type>::type>::type, wchar_t>::value
		, std::true_type, std::false_type >::type type;
	static constexpr typename type::value_type value = type::value;
	//using value = type::value;
};

template <typename F, typename... Args>
struct _is_invocable :
	std::is_constructible<
	std::function<void(Args ...)>,
	std::reference_wrapper<typename std::remove_reference<F>::type>
	>
{
};

template<typename Str>
struct is_primitive_string {
	typedef typename std::conditional<
		std::is_same<typename std::decay<typename std::remove_pointer<typename std::decay<Str>::type>::type>::type, char>::value ||
		std::is_same<typename std::decay<typename std::remove_pointer<typename std::decay<Str>::type>::type>::type, wchar_t>::value,
		std::true_type, std::false_type
	>::type type;
	static constexpr typename type::value_type value = type::value;
	using char_type = typename std::decay<typename std::remove_pointer<typename std::decay<Str>::type>::type>::type;
	using input_type = typename Str;
};
