#pragma once

// Papyrus - calls Papyrus functions from C++ (the game's own ones like Actor.SetRace and RaceMenu's
// CharGen.*), so Lazy Followers NG does exactly what the old scripts did. Every call goes through the VM's
// queue; a_done (optional) runs on the VM thread with the return value when the call has finished.

namespace Papyrus
{
    using Variable = RE::BSScript::Variable;
    using Done = std::function<void(const Variable&)>;

    // True when a game is loaded and no loading screen / main menu is up (calling the VM then is unsafe).
    bool GameReady();

    // Does the script a_class exist (installed .pex) and have the function a_fn (global or member, also
    // in its parents)? Calling a missing function crashes the VM, so every call checks this first.
    bool HasFunction(std::string_view a_class, std::string_view a_fn);

    // Low level: dispatch with ready-made arguments (deleted when the call is not made).
    bool Static(std::string_view a_class, std::string_view a_fn, RE::BSScript::IFunctionArguments* a_args, Done a_done = {});
    bool Method(RE::TESForm* a_form, std::string_view a_class, std::string_view a_fn, RE::BSScript::IFunctionArguments* a_args, Done a_done = {});

    // CallStatic("CharGen", "LoadCharacterEx", done, actor, race, name, flags)
    template <class... Args>
    bool CallStatic(std::string_view a_class, std::string_view a_fn, Done a_done, Args... a_args)
    {
        return Static(a_class, a_fn, RE::MakeFunctionArguments(std::move(a_args)...), std::move(a_done));
    }

    // CallMethod(actor, "Actor", "SetRace", done, race)
    template <class... Args>
    bool CallMethod(RE::TESForm* a_form, std::string_view a_class, std::string_view a_fn, Done a_done, Args... a_args)
    {
        return Method(a_form, a_class, a_fn, RE::MakeFunctionArguments(std::move(a_args)...), std::move(a_done));
    }

    // A list of Papyrus calls that run strictly one after another: each step starts when the previous one
    // has returned (the old scripts ran them in order; the VM would otherwise run separate calls in parallel).
    // Build it on any thread, then Run() it once. A step returning false (call not made) skips to the next.
    class Chain : public std::enable_shared_from_this<Chain>
    {
    public:
        using Step = std::function<bool(Done)>;  // start one call, pass it the Done it must report to

        static std::shared_ptr<Chain> Make() { return std::shared_ptr<Chain>(new Chain()); }

        Chain& Then(Step a_step)
        {
            _steps.push_back(std::move(a_step));
            return *this;
        }
        // A plain C++ step (runs on the VM thread when it is reached).
        Chain& ThenDo(std::function<void()> a_fn);

        void Run() { Next(); }

    private:
        Chain() = default;
        void Next();

        std::vector<Step> _steps;
        std::size_t       _pos = 0;
    };
}
