// Google Apps Script for the Vitool Box feedback form (script.google.com).
// Run setup() once; it creates the form, its response sheet and a daily trigger
// that keeps the app list in sync with catalog.json.

const CATALOG_URL = 'https://raw.githubusercontent.com/VitomDev/Vitool-Box/main/catalog.json';
const FIRST = 'Vitool Box';
const LAST = 'Other / General';

function setup() {
  const form = FormApp.create('Vitool Box · Bug reports & suggestions');
  form.setDescription('Found a bug or have an idea for any VitomDev app? Tell us here. Thanks for helping make the apps better!')
    .setConfirmationMessage('Thanks! Your message has been sent.')
    .setShowLinkToRespondAgain(true)
    .setProgressBar(false);

  const type = form.addMultipleChoiceItem().setTitle('What would you like to send?').setRequired(true);
  const app = form.addListItem().setTitle('Which app is it about?').setRequired(true);

  const bug = form.addPageBreakItem().setTitle('Bug report');
  form.addParagraphTextItem().setTitle('What happened?')
    .setHelpText('Describe the problem and what you expected to happen instead.').setRequired(true);
  form.addParagraphTextItem().setTitle('Steps to reproduce')
    .setHelpText('What did you do right before it happened? (optional)');
  form.addTextItem().setTitle('App version').setHelpText('You can see it in Vitool Box or in the app settings. (optional)');
  form.addTextItem().setTitle('Windows version').setHelpText('For example: Windows 11 24H2. (optional)');
  form.addTextItem().setTitle('Screenshot link').setHelpText('A link to an image or video of the problem. (optional)');
  addEmail(form);

  const idea = form.addPageBreakItem().setTitle('Suggestion')
    .setGoToPage(FormApp.PageNavigationType.SUBMIT);
  form.addParagraphTextItem().setTitle('Your idea').setRequired(true);
  form.addParagraphTextItem().setTitle('Why would it be useful?').setHelpText('(optional)');
  addEmail(form);

  type.setChoices([type.createChoice('Bug report', bug), type.createChoice('Suggestion', idea)]);

  const sheet = SpreadsheetApp.create('Vitool Box feedback (responses)');
  form.setDestination(FormApp.DestinationType.SPREADSHEET, sheet.getId());

  PropertiesService.getScriptProperties().setProperties({ formId: form.getId(), appItemId: String(app.getId()) });
  syncApps();

  ScriptApp.getProjectTriggers().forEach((t) => ScriptApp.deleteTrigger(t));
  ScriptApp.newTrigger('syncApps').timeBased().everyDays(1).atHour(6).create();

  info();
}

// Logs the links needed by the hub (public URL and the prefill one for the app question)
function info() {
  const props = PropertiesService.getScriptProperties();
  const form = FormApp.openById(props.getProperty('formId'));
  const app = form.getItemById(Number(props.getProperty('appItemId'))).asListItem();
  const prefill = form.createResponse().withItemResponse(app.createResponse(FIRST)).toPrefilledUrl();
  Logger.log('Form: ' + form.getPublishedUrl());
  Logger.log('Edit: ' + form.getEditUrl());
  Logger.log('Prefill: ' + prefill);
  Logger.log('Sheet: ' + SpreadsheetApp.openById(form.getDestinationId()).getUrl());
}

function addEmail(form) {
  form.addTextItem().setTitle('Your email')
    .setHelpText('Only if you want a reply. (optional)')
    .setValidation(FormApp.createTextValidation().requireTextIsEmail().build());
}

function syncApps() {
  const props = PropertiesService.getScriptProperties();
  const form = FormApp.openById(props.getProperty('formId'));
  const item = form.getItemById(Number(props.getProperty('appItemId'))).asListItem();

  const catalog = JSON.parse(UrlFetchApp.fetch(CATALOG_URL, { muteHttpExceptions: true }).getContentText());
  const names = catalog.apps.map((a) => a.manifest && a.manifest.name).filter(Boolean)
    .filter((n) => n !== FIRST && n !== LAST).sort((a, b) => a.localeCompare(b));
  const all = [FIRST, ...new Set(names), LAST];

  const current = item.getChoices().map((c) => c.getValue());
  if (current.join('\n') !== all.join('\n')) item.setChoiceValues(all);
}
