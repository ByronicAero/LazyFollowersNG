// Papyrus - dispatches Papyrus calls (the game's and RaceMenu's) from C++, with the safety checks the
// user's other mods use: never on the main menu / loading screens, never a function that is not there.

#include "Papyrus.h"

namespace Papyrus
{
    namespace
    {
        // Receives the return value of a Papyrus call and hands it to a C++ function.
        class Callback final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            explicit Callback(Done a_fn) :
                _fn(std::move(a_fn))
            {}
            void operator()(Variable a_result) override
            {
                if (_fn) {
                    _fn(a_result);
                }
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            Done _fn;
        };

        // True if a_type (or one of its parent scripts) has a global or member function a_fn.
        bool TypeHas(RE::BSScript::ObjectTypeInfo* a_type, std::string_view a_fn)
        {
            for (auto t = a_type; t; t = t->GetParent()) {
                if (auto funcs = t->GetGlobalFuncIter()) {
                    for (std::uint32_t i = 0; i < t->GetNumGlobalFuncs(); ++i) {
                        if (funcs[i].func && funcs[i].func->GetName() == a_fn) {
                            return true;
                        }
                    }
                }
                if (auto funcs = t->GetMemberFuncIter()) {
                    for (std::uint32_t i = 0; i < t->GetNumMemberFuncs(); ++i) {
                        if (funcs[i].func && funcs[i].func->GetName() == a_fn) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
    }

    bool GameReady()
    {
        auto ui = RE::UI::GetSingleton();
        if (!ui || !RE::PlayerCharacter::GetSingleton() || !RE::PlayerCharacter::GetSingleton()->Is3DLoaded()) {
            return false;
        }
        return !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    }

    bool HasFunction(std::string_view a_class, std::string_view a_fn)
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
        if (!vm->GetScriptObjectType(RE::BSFixedString(a_class), type) || !type) {
            return false;
        }
        return TypeHas(type.get(), a_fn);
    }

    bool Static(std::string_view a_class, std::string_view a_fn, RE::BSScript::IFunctionArguments* a_args, Done a_done)
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !GameReady() || !HasFunction(a_class, a_fn)) {
            logger::warn("Papyrus {}.{} not called (game not ready or function missing)", a_class, a_fn);
            delete a_args;
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new Callback(std::move(a_done)));
        return vm->DispatchStaticCall(RE::BSFixedString(a_class), RE::BSFixedString(a_fn), a_args, cb);
    }

    bool Method(RE::TESForm* a_form, std::string_view a_class, std::string_view a_fn, RE::BSScript::IFunctionArguments* a_args, Done a_done)
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!a_form || !vm || !GameReady() || !HasFunction(a_class, a_fn)) {
            logger::warn("Papyrus {}.{} not called (no object, game not ready or function missing)", a_class, a_fn);
            delete a_args;
            return false;
        }
        auto policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            delete a_args;
            return false;
        }
        const auto handle = policy->GetHandleForObject(a_form->GetFormType(), a_form);
        if (handle == policy->EmptyHandle()) {
            logger::warn("Papyrus {}.{}: no handle for {:08X}", a_class, a_fn, a_form->GetFormID());
            delete a_args;
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new Callback(std::move(a_done)));
        return vm->DispatchMethodCall(handle, RE::BSFixedString(a_class), RE::BSFixedString(a_fn), a_args, cb);
    }

    Chain& Chain::ThenDo(std::function<void()> a_fn)
    {
        return Then([fn = std::move(a_fn)](Done a_done) {
            fn();
            a_done(Variable{});
            return true;
        });
    }

    // Starts the next step; its Done starts the one after. Steps that could not be called are skipped.
    void Chain::Next()
    {
        while (_pos < _steps.size()) {
            auto& step = _steps[_pos++];
            auto  self = shared_from_this();
            if (step([self](const Variable&) { self->Next(); })) {
                return;  // the Done of this step continues the chain
            }
        }
    }
}
