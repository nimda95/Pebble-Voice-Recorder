// Keep in sync with src/c/lang.c.
module.exports = [
  ['en', 'English'], ['ar', 'Arabic'], ['fr', 'French'], ['es', 'Spanish'], ['de', 'German'],
  ['it', 'Italian'], ['pt', 'Portuguese'], ['nl', 'Dutch'], ['tr', 'Turkish'], ['ru', 'Russian'],
  ['uk', 'Ukrainian'], ['pl', 'Polish'], ['cs', 'Czech'], ['ro', 'Romanian'], ['hu', 'Hungarian'],
  ['el', 'Greek'], ['sv', 'Swedish'], ['da', 'Danish'], ['no', 'Norwegian'], ['fi', 'Finnish'],
  ['he', 'Hebrew'], ['fa', 'Persian'], ['hi', 'Hindi'], ['ur', 'Urdu'], ['id', 'Indonesian'],
  ['ms', 'Malay'], ['vi', 'Vietnamese'], ['th', 'Thai'], ['zh', 'Chinese'], ['ja', 'Japanese'],
  ['ko', 'Korean'], ['ca', 'Catalan']
].map(function (l) { return { code: l[0], name: l[1] }; });
