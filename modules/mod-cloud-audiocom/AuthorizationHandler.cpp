/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  AuthorizationHandler.cpp

  Dmitry Vedenko

**********************************************************************/
#include "AuthorizationHandler.h"

#include <chrono>
#include <future>
#include <optional>

#include "OAuthService.h"
#include "ServiceConfig.h"

#include "ui/dialogs/LinkAccountDialog.h"
#include "ui/dialogs/LinkFailedDialog.h"
#include "ui/dialogs/LinkSucceededDialog.h"
#include "ui/dialogs/WaitForActionDialog.h"

#include "CodeConversions.h"
#include "HelpSystem.h"
#include "MemoryX.h"

namespace cloud::audiocom
{
namespace
{
AuthorizationHandler handler;

std::optional<AuthResult> WaitForAuth(
   std::future<std::optional<AuthResult>> future,
   const AudacityProject* project, TranslatableString dialogMessage = {})
{
   using namespace sync;

   if (
      future.wait_for(std::chrono::milliseconds { 100 }) !=
      std::future_status::ready)
   {
      auto waitResult =
         WaitForActionDialog { project, dialogMessage }.ShowDialog(
            [&future]() -> DialogButtonIdentifier
            {
               if (
                  future.wait_for(std::chrono::milliseconds { 50 }) !=
                  std::future_status::ready)
                  return {};

               return { L"done" };
            });

      if (waitResult == WaitForActionDialog::CancellButtonIdentifier())
         return AuthResult { AuthResult::Status::Cancelled, {} };
   }

   if (GetOAuthService().HasAccessToken())
      return AuthResult { AuthResult::Status::Authorised, {} };

   return future.get();
}
} // namespace

AuthorizationHandler& GetAuthorizationHandler()
{
   return handler;
}

AuthResult PerformBlockingAuth(
   AudacityProject* project, const TranslatableString& alternativeActionLabel)
{
   using namespace sync;
   auto& oauthService = GetOAuthService();

   // Assume, that the token is valid
   // Services will need to handle 403 errors and refresh the token
   if (GetOAuthService().HasAccessToken())
      return { AuthResult::Status::Authorised, {} };

   GetAuthorizationHandler().PushSuppressDialogs();
   auto popSuppress =
      finally([] { GetAuthorizationHandler().PopSuppressDialogs(); });

   if (oauthService.HasRefreshToken())
   {
      std::promise<std::optional<AuthResult>> promise;

      oauthService.ValidateAuth(
         [&promise](auto...) { promise.set_value({}); }, true);

      if (auto waitResult = WaitForAuth(promise.get_future(), project))
         return *waitResult;
   }

   auto linkResult =
      sync::LinkAccountDialog { project, alternativeActionLabel }.ShowDialog();

   if (linkResult == LinkAccountDialog::CancellButtonIdentifier())
      return { AuthResult::Status::Cancelled, {} };

   if (linkResult == LinkAccountDialog::AlternativeButtonIdentifier())
      return { AuthResult::Status::UseAlternative, {} };

   std::promise<std::optional<AuthResult>> promise;

   auto authSubscription = oauthService.Subscribe(
      [&promise](auto& result)
      {
         promise.set_value(
            result.authorised ?
               AuthResult { AuthResult::Status::Authorised, {} } :
               AuthResult { AuthResult::Status::Failure,
                            std::string(result.errorMessage) });
      });

   OpenInDefaultBrowser(
      { audacity::ToWXString(GetServiceConfig().GetOAuthLoginPage()) });

   auto waitResult = WaitForAuth(
      promise.get_future(), project, XO("Please, complete action in browser"));

   if (waitResult)
      return *waitResult;

   return AuthResult { AuthResult::Status::Failure, {} };
}

AuthorizationHandler::AuthorizationHandler()
    : mAuthStateChangedSubscription(GetOAuthService().Subscribe(
         [this](const auto& message) { OnAuthStateChanged(message); }))
{
}

void AuthorizationHandler::PushSuppressDialogs()
{
   ++mSuppressed;
}

void AuthorizationHandler::PopSuppressDialogs()
{
   assert(mSuppressed > 0);

   if (mSuppressed > 0)
      --mSuppressed;
}

void AuthorizationHandler::OnAuthStateChanged(
   const AuthStateChangedMessage& message)
{
   if (mSuppressed > 0 || message.silent)
      return;

   if (!message.errorMessage.empty())
   {
      LinkFailedDialog dialog { nullptr };
      dialog.ShowModal();
   }
   else if (message.authorised)
   {
      LinkSucceededDialog dialog { nullptr };
      dialog.ShowModal();
   }
}
} // namespace cloud::audiocom
