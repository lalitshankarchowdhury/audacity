/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  AudioComDialogBase.cpp

  Dmitry Vedenko

**********************************************************************/
#include "AudioComDialogBase.h"

#include <cassert>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "Prefs.h"
#include "Project.h"
#include "ProjectWindow.h"

#include "AccessibleLinksFormatter.h"
#include "ShuttleGui.h"

namespace audacity::cloud::audiocom::sync
{
namespace
{
wxWindow* GetProjectWindow(const AudacityProject* project)
{
   if (project == nullptr)
      return nullptr;

   return &ProjectWindow::Get(*const_cast<AudacityProject*>(project));
}

wxString GetOptionalPrefsIdentifier(const DialogIdentifier& identifier)
{
   if (identifier.empty())
      return {};

   return wxString::Format("/cloud/audiocom/%s/skip", identifier.GET());
}
} // namespace

DialogButtonIdentifier audacity::cloud::audiocom::sync::AudioComDialogBase::ShowDialog(
   std::function<DialogButtonIdentifier()> poller)
{
   mDialogSizer->AddStretchSpacer(1);

   if (HasSeparator())
   {
      mDialogSizer->Add(
         safenew wxStaticLine { this },
         wxSizerFlags {}.Border(wxTOP, 16).Expand());
      mDialogSizer->AddSpacer(8);
   }
   else
      mDialogSizer->AddSpacer(16);

   mDialogSizer->Add(
      mButtonSizer, wxSizerFlags {}.Border(wxLEFT | wxRIGHT, 16).Expand());
   mDialogSizer->AddSpacer(8);

   SetSizerAndFit(mDialogSizer);
   Center();

   // We cannot do it earlier to avoid licking the sizers and children
   if (!mOptionalPrefsIdentifier.empty())
   {
      if (gPrefs->ReadBool(mOptionalPrefsIdentifier.GET(), false))
         return mEscButtonIdentifier;
   }

   Show();
   Raise();

   wxWindowDisabler disabler(this);

   if (!poller)
      poller = [] { return DialogButtonIdentifier {}; };

   while (IsShown())
   {
      wxYield();

      if (auto result = poller(); !result.empty())
         return result;
   }

   // mResultButtonIdentifier is empty if the dialog was closed using the
   // cross button
   return mResultButtonIdentifier.empty() ? mEscButtonIdentifier :
                                            mResultButtonIdentifier;
}

DialogButtonIdentifier AudioComDialogBase::CancellButtonIdentifier()
{
   return { L"Cancel" };
}

AudioComDialogBase::AudioComDialogBase(
   const AudacityProject* project,
   const DialogIdentifier& optionalPrefsIdentifier)
    : wxDialogWrapper { GetProjectWindow(project), wxID_ANY,
                        XO("Save to audio.com") }
    , mProject { project }
    , mOptionalPrefsIdentifier { GetOptionalPrefsIdentifier(
         optionalPrefsIdentifier) }
    , mDialogSizer { new wxBoxSizer(wxVERTICAL) }
    , mButtonSizer { new wxBoxSizer(wxHORIZONTAL) }
{
   mDialogSizer->SetMinSize({ 420, 140 });
   if (!mOptionalPrefsIdentifier.empty())
   {
      const auto skipDialog =
         gPrefs->ReadBool(mOptionalPrefsIdentifier.GET(), false);

      auto checkbox =
         safenew wxCheckBox { this, wxID_ANY,
                              XO("Don't show this again").Translation() };

      checkbox->SetValue(skipDialog);

      mButtonSizer->Add(checkbox, wxSizerFlags {}.CenterVertical());

      checkbox->Bind(
         wxEVT_CHECKBOX,
         [this, checkbox](auto&)
         {
            gPrefs->Write(mOptionalPrefsIdentifier.GET(), checkbox->GetValue());
            gPrefs->Flush();
         });
   }

   mButtonSizer->AddStretchSpacer();

   Bind(
      wxEVT_CHAR_HOOK,
      [this](auto& evt)
      {
         if (!IsEscapeKey(evt))
         {
            evt.Skip();
            return;
         }

         EndDialog(mEscButtonIdentifier);
      });
}

void AudioComDialogBase::AddTitle(const TranslatableString& title)
{
   auto font = GetFont().Bold();

   font.SetFractionalPointSize(font.GetFractionalPointSize() * 1.5f);

   auto statText = safenew wxStaticText { this, wxID_ANY, title.Translation() };

   statText->SetFont(font);

   mDialogSizer->AddSpacer(16);
   mDialogSizer->Add(statText, wxSizerFlags {}.Border(wxLEFT | wxRIGHT, 16));
}

void AudioComDialogBase::AddParagraph(const TranslatableString& paragraph)
{
   auto statText =
      safenew wxStaticText { this, wxID_ANY, paragraph.Translation() };

   mDialogSizer->AddSpacer(16);
   mDialogSizer->Add(statText, wxSizerFlags {}.Border(wxLEFT | wxRIGHT, 16));

   statText->Wrap(400);
}

void AudioComDialogBase::AddParagraphWithLink(
   const TranslatableString& paragraph, const wxString& placeholder,
   const wxString& urlText, const std::string& url)
{
   auto panel = safenew wxPanelWrapper { this, wxID_ANY };

   panel->SetMaxSize({ 400, -1 });

   ShuttleGui S { panel, eIsCreating, true, { 400, -1 } };
   S.SetBorder(0);
   S.StartVerticalLay();
   {
      AccessibleLinksFormatter formatter { paragraph };
      formatter.FormatLink(placeholder, Verbatim(urlText), url);
      formatter.Populate(S);
   }
   S.EndVerticalLay();

   panel->Layout();
   panel->Fit();

   mDialogSizer->AddSpacer(16);
   mDialogSizer->Add(panel, wxSizerFlags {}.Border(wxLEFT | wxRIGHT, 16));
}

void AudioComDialogBase::AddButton(
   DialogButtonIdentifier identifier, const TranslatableString& text, int type)
{
   auto button = safenew wxButton { this, wxID_ANY, text.Translation() };

   mButtonSizer->Add(button, wxSizerFlags {}.Border(wxLEFT, 8));

   if (type & EscButton)
      mEscButtonIdentifier = identifier;

   button->Bind(
      wxEVT_BUTTON, [this, identifier = std::move(identifier)](auto&)
      { EndDialog(identifier); });

   if (type & DefaultButton)
      button->SetDefault();
}

void AudioComDialogBase::SetDialogTitle(const TranslatableString& dialog)
{
   SetTitle(dialog);
}

bool AudioComDialogBase::HasSeparator() const
{
   return true;
}

void AudioComDialogBase::EndDialog(DialogButtonIdentifier identifier)
{
   mResultButtonIdentifier = std::move(identifier);
   Close();
}

} // namespace audacity::cloud::audiocom::sync
