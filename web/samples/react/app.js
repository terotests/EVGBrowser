// A small React 18 app (React's UMD builds, no bundler): state, effects,
// a controlled field, a list with keys, a timer, clicks that bubble.
var h = React.createElement;

function Item(props) {
  return h('div', { className: 'item' + (props.done ? ' done' : ''), onClick: props.onToggle },
    h('span', null, (props.done ? '☑ ' : '☐ ') + props.text),
    h('span', { className: 'count' }, props.done ? 'tehty' : ''));
}

function Clock() {
  var s = React.useState(0), secs = s[0], setSecs = s[1];
  React.useEffect(function () {
    var id = setInterval(function () { setSecs(function (n) { return n + 1; }); }, 1000);
    return function () { clearInterval(id); };
  }, []);
  return h('p', { className: 'count' }, 'Sivu on ollut auki ' + secs + ' s');
}

function App() {
  var t = React.useState(''), text = t[0], setText = t[1];
  var it = React.useState([
    { id: 1, text: 'Lue kirjaa', done: false },
    { id: 2, text: 'Ulkoile koiran kanssa', done: true },
    { id: 3, text: 'Piirrä kuva mummolle', done: false }
  ]), items = it[0], setItems = it[1];
  var f = React.useState('kaikki'), filter = f[0], setFilter = f[1];
  var nextId = React.useRef(4);

  function add() {
    if (!text.trim()) return;
    setItems(items.concat([{ id: nextId.current++, text: text.trim(), done: false }]));
    setText('');
  }
  function toggle(id) {
    setItems(items.map(function (x) { return x.id === id ? { id: x.id, text: x.text, done: !x.done } : x; }));
  }
  var shown = items.filter(function (x) {
    return filter === 'kaikki' || (filter === 'tehdyt' ? x.done : !x.done);
  });
  var left = items.filter(function (x) { return !x.done; }).length;

  return h('div', { className: 'todo' },
    h('h1', null, 'Tehtävälista'),
    h(Clock),
    h('form', { className: 'row', onSubmit: function (e) { e.preventDefault(); add(); } },
      h('input', { value: text, placeholder: 'Uusi tehtävä', onChange: function (e) { setText(e.target.value); } }),
      h('button', { type: 'submit' }, 'Lisää')),
    h('div', { className: 'row' }, ['kaikki', 'tekemättä', 'tehdyt'].map(function (name) {
      return h('span', { key: name, className: 'chip' + (filter === name ? ' on' : ''), onClick: function () { setFilter(name); } }, name);
    })),
    shown.map(function (x) {
      return h(Item, { key: x.id, text: x.text, done: x.done, onToggle: function () { toggle(x.id); } });
    }),
    h('p', { className: 'count' }, left === 0 ? 'Kaikki tehty!' : left + ' tekemättä'));
}

ReactDOM.createRoot(document.getElementById('root')).render(h(App));
