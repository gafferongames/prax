# Mermaid

`lib/mermaid` draws diagrams written in Mermaid's language (`pr/mermaid.h`). It parses a diagram, shapes its text
with `lib/text` and lays it out with `lib/graph` ([graph.md](graph.md)) once, when the diagram is created. A draw
records the diagram into a `lib/draw` list in a light or a dark color scheme, and needs no layout. It does not depend
on any document layout. A client places the diagram's extent wherever it likes and draws it there.

Diagrams reach documents as images. Markdown writes a `mermaid` fence as an `<img>` of type `text/vnd.mermaid`
([markdown.md](markdown.md)), and lib/html draws an image of that type with lib/mermaid, in the fonts of the image's
document, at the diagram's natural size. The image draws in its box's used color scheme, which layout gives every
painter, as it gives an SVG image.

## Which behavior

Mermaid has no specification. Its documentation and mermaid.js define the language, and the engine accepts what they
accept. What the engine draws is its own decision:

- **The language is mermaid.js's.** A source that mermaid.js renders should parse. Its shapes, links, labels,
  subgraphs and styles mean what they mean in mermaid.js.
- **The look is Mermaid's classic look** (`look: classic`): its colors, its font list, and the sizes of its nodes and
  links. Diagrams then look like the Mermaid that readers have seen for years. Mermaid 12 made a new look the default,
  and sites that embed Mermaid adopt it as they upgrade.
- **The page chooses the theme.** A diagram is drawn in the color scheme of what shows it: Mermaid's default theme in
  a light scheme, and in a dark one the engine's dark theme, the default's counterpart with its indigo nodes and
  olive subgraphs darkened. Mermaid's own dark themes are gray, or are made for its new look. A source that names a
  theme in its front matter or an init directive draws in dark if it names dark and in light otherwise. A sequence
  diagram also reads the `wrap` setting. The rest of a source's configuration is ignored, since diagrams in documents
  rarely set it.
- **Every kind reads its source the same way.** Front matter, directives, which may span lines, and comments come
  before the header, and the comments, directives and accessibility statements in the body are removed before a
  kind's grammar reads it, as mermaid.js removes them (`mermaid_parse.h`).
- **A source that does not draw shows why.** As mermaid.js does, a host can draw a diagram in place of the source that
  says it failed, and where.
- **The layout is the engine's.** Where mermaid.js and the engine place nodes and route edges differently, the engine
  uses the layout that `docs/graph.md` describes and does not change it to match dagre.
- **A subgraph keeps its direction only while its nodes link among themselves.** As in mermaid.js, a subgraph whose
  nodes link to nodes outside it lays out in its parent's direction. A layered layout puts unlinked nodes in the same
  layer, so an LR subgraph's unlinked nodes would stack in a column. Authors of such subgraphs usually want the row
  that the parent's direction gives.
- **The first node written starts the flow.** If links form a cycle, the links that close it are the ones that a walk
  in source order finds closing it, as in mermaid.js.

## Diagrams

The engine draws flowcharts (`graph` and `flowchart`), sequence diagrams (`sequenceDiagram`), class diagrams
(`classDiagram`), state diagrams (`stateDiagram`), entity relationship diagrams (`erDiagram`), requirement diagrams
(`requirementDiagram`), gantt charts (`gantt`), timelines (`timeline`), user journeys (`journey`), pie charts (`pie`),
quadrant charts (`quadrantChart`), XY charts (`xychart`), git graphs (`gitGraph`) and mindmaps (`mindmap`). A source
that names another kind of diagram fails with NotSupported. A source that names no diagram, or does not parse, fails
with Syntax and reports where.

### Flowcharts

- **Nodes** take the shapes that their brackets select, and are sized around their labels with Mermaid's padding.
- **Metadata** after a node's name, as in `A@{ shape: cyl, label: "Disk" }`, selects any of mermaid.js's named shapes
  by its short name or an alias, and sets the label, which is Markdown. The block may follow the node's brackets or
  classes, may span lines, and may name a node within a chain of links. The engine accepts and ignores the keys that it
  does not draw, such as `icon`, `img`, `pos`, `w` and `h`. A shape that mermaid.js does not name is an error. Each
  shape is sized and drawn as mermaid.js's classic look draws it, and the small markers (`sm-circ`, `fr-circ`,
  `f-circ`, `cross-circ`, `fork`, `hourglass` and `bolt`) draw no label, as in mermaid.js.
- **Keywords are lowercase**, as mermaid.js reads them, so that `End` and `Class` name nodes.
- **Links** have a stroke (normal, thick, dotted or invisible), marks at either end (arrows, circles or crosses), a
  length in layers, and an optional label, written within the stroke or between bars. A link spans at most 16 layers,
  however long its stroke, so that a source cannot make a diagram of any height.
- **Named links.** A name before a stroke, as in `A e1@--> B`, names the first link that the statement makes, as in
  mermaid.js, unless another link has the name. The name is the link's item ID (`pr/mermaid.h`), a `class` statement
  may style the link by it, and its metadata, as in `e1@{ animate: true }`, is accepted and ignored, since the engine
  draws no animation.
- **Subgraphs** are layout groups, with their titles in their padding. They may set their own direction, as described
  above. A link may join a subgraph just as it joins a node. A link between a subgraph and one of its own members is
  not drawn. Subgraphs nest at most 64 deep: a layered layout's cost grows with depth, and documents nest a few
  levels.
- **Labels** decode quotes, `<br>` line breaks and Mermaid's entity codes (`#quot;`, `#9829;`). A label is HTML, whose
  tags for emphasis, code, color and decoration style its text. A Markdown string, between backticks, is Markdown
  that lib/markdown reads, and its lines wrap at 120 px, as mermaid.js 12 wraps them.
- **Styles** come from `classDef`, `class`, `:::` and `style`, in that order of precedence, applied on top of the
  `default` class. As in mermaid.js, `class` styles only the nodes and links that the source has made, and makes none.
  A link's style is its classes, then its `linkStyle`, which replaces any earlier one for that link, or else the
  `linkStyle default`. An index past the links made so far is an error, as in mermaid.js. The engine draws fill,
  stroke, stroke width, text color and dashes. Colors are CSS colors (`pr/color_css.h`).
- **Links to pages.** `click A "url"` and `click A href "url"` give the node its item's link, for a host to follow.
  As in mermaid.js, the statement names a node that the source has already made, a URL whose scheme is javascript,
  vbscript or data gives no link, and a callback, which the engine does not call, gives none.
- **Links meet their nodes at separate points.** Each end of a link has its own port. The layout spreads the ports
  along the side of the node that faces the link, in the order that avoids crossings. The diagram then extends each
  end along its route to the outline of the node's shape.
- **A title** in the front matter is drawn above the diagram.

### Sequence diagrams

The layout is mermaid.js's sequence renderer, step by step, so that a diagram has the reference's proportions.
Keywords ignore case, as mermaid.js reads them.

- **Participants** stand side by side, and again at the bottom, joined by their lifelines. A participant is a box,
  `actor` draws a stick figure, and the `type` in a participant's metadata selects a boundary, control, entity,
  database, collections or queue. A message or note that names a participant first declares it. The gap after a
  participant makes room for the widest message or note between it and its next neighbor.
- **Created and destroyed participants.** After `create`, the next message creates its target, which starts at that
  message, centered on its line. After `destroy`, the next message ends its participant there. Each message meets the
  participant at its box or glyph.
- **Messages** run down the page in source order, with the line and heads of their arrow. A `+` or `-` after the arrow,
  or an `activate` or `deactivate` statement, starts or ends an activation, which messages meet at its edge. At most
  64 activations are open at once.
- **Notes** sit left of, right of, or over one or two participants.
- **Blocks** (`loop`, `alt`, `opt`, `par`, `critical`, `break`) frame their statements, with their sections' dashed
  lines and titles, and grow around what is nested in them. `rect` colors a background instead. Blocks nest at most 64
  deep. The layout grows every open block and activation around each statement, as mermaid.js's does, so the limits
  bound its cost.
- **Boxes** group participants behind a color and a title. `autonumber` numbers the messages, and a title comes from
  the front matter or a `title` statement.
- **Wrapped text** fits the space that the layout gives it, and the layout does not grow to fit the text. A message
  wraps at the width between its participants, a note at its own width, and a participant's name at the width of a
  participant. A `wrap:` or `nowrap:` prefix decides for one message, note, name or box title. Without a prefix, the
  `wrap` setting decides, and text does not wrap by default.

### Class diagrams

lib/graph lays out a class diagram, as it lays out a flowchart, and the classes are sized as Mermaid sizes them.
Keywords are case-sensitive, as mermaid.js reads them.

- **Classes** are boxes of three compartments: the annotation and the name, then the attributes, then the methods. A
  member is a method if it holds a closing parenthesis. A method shows its return type after a colon. A static member
  is underlined, an abstract one is italic, and generic types written between tildes show in angle brackets.
- **Relations** join two classes with a solid or dashed line. Each end has its own mark: a hollow triangle for
  inheritance or realization, a filled or hollow diamond for composition or aggregation, an arrowhead for a dependency,
  or a circle for a lollipop. The name at a lollipop's far end is an interface, drawn as its name alone. A relation's
  label sits at its middle, and its cardinalities sit beside its ends.
- **Namespaces** group their classes in a box with their name. As in mermaid.js, a namespace that no relation crosses
  lays out across its parent's direction, so a diagram that runs down shows the namespace's classes in a row.
  Namespaces nest at most 64 deep.
- **Notes** stand alone or attach to a class with a dotted line.
- **Styles** come from `classDef`, `cssClass`, `:::` and `style`, as in flowcharts. As in mermaid.js, `cssClass` and
  `style` style only the classes that the source has already named, and name none.
- **Links to pages.** `click A href "url"` and `link A "url"` give the class its item's link, for a host to follow. As
  in mermaid.js, the statement applies to a class that the source has already named, a URL whose scheme is
  javascript, vbscript or data gives no link, and a callback (`click A call fn()` or `callback`), which the engine
  does not call, gives none.

### State diagrams

lib/graph lays out a state diagram, and the states are sized as Mermaid sizes them. Keywords ignore case, as mermaid.js
reads them.

- **States** are rounded boxes. A state's descriptions, which `State : text` adds, sit below its name and a divider.
- **Pseudostates.** `[*]` is the start of its scope as a transition's source, and its end as a target. `<<fork>>` and
  `<<join>>` are bars across the flow, and `<<choice>>` is a diamond. Transitions meet a start, an end or a choice at its
  outline, and end in an arrowhead.
- **Composite states** hold their states below a band with their name. Each has its own start and end, and may set its
  own direction. `--` divides one into concurrent regions, which lie side by side in dashed boxes, and a composite
  state that opens again continues in its last region. Composite states nest at most 64 deep. A transition between a
  composite state and a state inside it is not drawn, as in flowcharts.
- **Notes** sit beside their state, joined to it by a dashed line.
- **Styles** come from `classDef`, `class`, `:::` and `style`, as in flowcharts.
- **Links to pages.** `click State "url"` and `click State href "url"` give the state its item's link, as in
  flowcharts.

### Entity relationship diagrams

lib/graph lays out an ER diagram, and the entities are sized as Mermaid sizes them. Keywords and cardinality marks
ignore case, as mermaid.js reads them.

- **Entities** with attributes are tables: a header with the entity's name, then a row for each attribute, in
  alternating fills. The columns hold the type, the name, the keys and the comment, and a column that no attribute fills
  is left out. An entity without attributes is a box around its name, which wraps at 120 px. An alias in brackets
  replaces the name.
- **Relationships** are solid if they are identifying (`--`) and dashed otherwise (`..`), with their label at their
  middle. Each end carries crow's foot marks for its cardinality: a bar for one, a circle for zero, and a crow's foot
  for many. The end of a route runs straight for the length of its marks, so that they lie along it.
- **Styles** come from `classDef`, `class`, `:::` and `style`, as in flowcharts. As in mermaid.js, `class` and `style`
  style only the entities that the source has already named, and name none.

### Gantt charts

The layout is mermaid.js's gantt renderer: a row for each task, banded by section, and a bar from the task's start to its
end on a time axis that spans every task. The chart is 800 px wide, which mermaid.js would take from its container.

- **Times** are in a calendar without time zones, so that a date means the same time everywhere. `dateFormat` reads
  dates with dayjs's tokens, and an ISO date always reads. A task starts at a date, `after` other tasks, or where the
  task before it ends. It ends at a date, `until` another task starts, or after a duration (`ms`, `s`, `m`, `h`, `d`,
  `w`, `M` or `y`). Tasks may name tasks that come after them. If two tasks have the same ID, the later one counts, as
  in mermaid.js. Tasks whose times depend on each other fail with Syntax.
- **Excluded days**, from `excludes` (`weekends`, weekday names, or dates), lengthen the tasks with durations that span
  them, and are shaded. `weekend friday` moves the weekend. `inclusiveEndDates` includes a task's end date.
- **Tasks** are bars in the colors of their tags (`done`, `active`, `crit`). A `milestone` is a diamond at its start. A
  task's name is inside its bar if it fits, and beside it otherwise.
- **Vert markers** (`Release : vert, v1, 2024-01-10, 4m`, with or without an ID) take no row. As in mermaid.js 12, a
  marker is a line at its start from the top of the grid to past the rows, in the theme's `vertLineColor`, with its name
  centered below it at 15 px.
- **The axis** has about ten ticks, on the calendar interval that d3's time scale would choose, unless `tickInterval`
  sets one. `axisFormat` writes them in a d3 time format, and `weekday` sets the day that weekly ticks fall on.
  `topAxis` repeats the labels above the tasks.
- **The grid** is light grey, as mermaid.js's theme says. mermaid.js itself draws it nearly black, since an attribute
  overrides its theme's color.
- **Not drawn:** the today marker, which would make a document's chart change from day to day, and `displayMode
  compact`.

### Pie charts

The layout is mermaid.js's: a pie of radius 185 in a square of 450, with a legend to its right. The slices go clockwise
from the top in source order, in the theme's twelve pie colors in turn, each with its percentage, rounded, inside it.
`showData` adds each slice's value to the legend. The dark theme's pie colors are darker shades of the light theme's.

### Git graphs

The layout is mermaid.js's, left to right: a dashed lane for each branch, in the order of the branches' `order` and
then of their creation, and a column for each commit, in the order of the commands. Lines join each commit to its
parents, and turn with rounded corners at the parent's column, or at the child's for a merge's second parent. A line
that would run along a lane past another commit on it detours along a free height between the lanes.

- **Commands** are mermaid.js's: `commit`, `branch`, `checkout` or `switch`, `merge` and `cherry-pick`, with their
  `id`, `type`, `tag`, `order` and `parent` attributes. Commands that mermaid.js forbids, such as merging a branch
  into itself, fail with Syntax.
- **Commits** are circles in their branch's color: a cross for `REVERSE`, a square in a square for `HIGHLIGHT`, a ring
  for a merge, and a cherry for a cherry-pick, which is tagged with the commit that it picks. A commit's ID is below
  it, turned 45 degrees, and its tags are above it. A commit without an ID of its own gets one in mermaid.js's form,
  whose digits come from the commit's number, so that the graph draws alike each time.
- **Not drawn:** the `TB` and `BT` orientations, which draw left to right, and parallel commits.

### Mindmaps

lib/graph lays out a mindmap as a tree down the page, as mermaid.js's classic look lays it out with dagre. A node's
indentation makes it a child of the nearest node before it that is indented less, and a mindmap has one root.

- **Nodes** take the shapes of their brackets: `[square]`, `(rounded)`, `((circle))`, `))bang((`, `)cloud(` and
  `{{hexagon}}`. A node without brackets is a rounded box with a line along its bottom. Text wraps at 200 px, and may be
  a Markdown string.
- **Sections.** Each of the root's children starts a section, which colors its nodes and the lines to them in the
  theme's next section color. The lines thin with depth.
- **Not drawn:** icons, and the classes that `:::` gives a node.

### XY charts

The layout is mermaid.js's, in a chart of 700 by 500: the plot keeps half of each dimension, the title and the axes
take what they need from the rest, and the plot takes what remains. `xychart` and `xychart-beta` draw alike.
`horizontal` puts the x-axis down the left and the y-axis along the top.

- **Axes.** The x-axis is a band of categories, `[a, b, "c d"]`, or a linear range, `min --> max`. Without either,
  it numbers the first series's values from 1. The y-axis is linear, over its range or over the values of every
  series. A linear axis's ticks are d3's: about ten round values. Each axis has a line, ticks, labels and an optional
  title, each drawn only if it fits.
- **Series.** `bar` and `line` series draw in source order, each in the theme's next plot color. Bars stand on the
  plot's edge, and on a band axis they share each category's place, so a later series draws over an earlier one.
  Bars widen the x-axis's padding so that the first and last bars fit in the plot.
- **Not drawn:** the legend of series' titles, data labels, line point labels, and configuration such as the chart's
  size or label rotation.

### User journeys

The layout is mermaid.js's: the tasks stand in a row in source order, 150 px wide and 200 px apart, with an arrow
along the row below them.

- **Sections** are headers above their tasks, and a header spans its tasks. Each section takes the next of the theme's
  seven fill colors, which its header and its tasks share.
- **Tasks** are boxes, with a dot in each actor's color along their top edge. A dashed line runs down from each task to
  a face that shows the task's score. A score of 5 puts the face highest, and each point less moves it 30 px down. The
  face smiles above 3, frowns below 3, and is neutral at 3.
- **The legend** lists the actors at the left, sorted by name, each with a dot in its color. As in mermaid.js, a legend
  wider than half the left margin moves the tasks right by its width.

### Timelines

The layout is mermaid.js's classic timeline: the periods stand in a row in source order, 190 px wide and 200 px apart,
above an arrow. Each period's events stack below the arrow, along a dashed line down from the period.

- **Sections** are headers above their periods, and a header spans its periods. Each section and its periods and
  events take the next of the theme's twelve section colors. Without sections, each period takes the next color. As in
  mermaid.js, the periods before the first section are not drawn if there are sections.
- **Boxes** have a line in a contrasting color along their bottom. Events are brighter than their periods, as
  mermaid.js's brightness filter makes them.
- **Text** wraps at 150 px. Every section is as tall as the tallest, and so is every period. An event is as tall as its
  text, and at least 50 px.
- **Events** follow a colon and a space, and a line that starts with a colon adds events to the period before it. A
  colon that no space follows is part of an event's text.
- **Not drawn:** the `TD` direction, which draws as `LR`, and `disableMulticolor`.

### Requirement diagrams

lib/graph lays out a requirement diagram, and the blocks are sized as Mermaid sizes them. Keywords, and the values of
risks and verification methods, ignore case, as mermaid.js reads them.

- **Blocks** are boxes with their kind between double angle brackets and their name in bold, centered, then a divider
  and their fields, one to a line. A requirement (`requirement`, `functionalRequirement`, `interfaceRequirement`,
  `performanceRequirement`, `physicalRequirement` or `designConstraint`) shows its ID, text, risk and verification
  method. An `element` shows its type and document reference. A field that the source leaves out is not shown, and a
  block without fields has no divider. If two blocks have the same name, the first one counts.
- **Relations** (`a - satisfies -> b`, or `b <- satisfies - a`) are labeled with their kind. `contains` is a solid line
  with a circled cross at the containing block. The others are dashed, with an open arrowhead at their target. A
  relation that names a block that the source does not define fails with Syntax, since mermaid.js cannot draw it.
- **Styles** come from `classDef`, `class`, `:::` and `style`, as in flowcharts. A `class` or `style` statement may
  come before the block that it names, and one that names no block is ignored.

### Quadrant charts

The layout is mermaid.js's: a square of 500, with the title at the top and the y axis's labels at the left, turned to
read upward. The x axis's labels are at the bottom if the chart has points, and at the top otherwise. The four
quadrants fill the rest, numbered counterclockwise from the top right, each in its own shade of the theme's primary
color. A quadrant's label is at its top if the chart has points, and at its middle otherwise.

- **Axes** take two labels (`x-axis Low --> High`), each centered on its half, or one, which starts at the axis's
  start.
- **Points** (`Name: [x, y]`) have coordinates from 0 to 1, with their names below them. `radius`, `color`,
  `stroke-color` and `stroke-width` style a point, after the styles of the class that `:::` gives it, which `classDef`
  defines. A style that mermaid.js rejects, such as a color that is not a hex code, fails with Syntax.
- **A point without a color** is drawn in the color of the chart's text. mermaid.js's theme gives it no valid color,
  and browsers draw it black.

## Testing

- `lib/mermaid/tests` checks the parser on the language's constructs and errors, and checks that diagrams lay out and
  draw.
- `suites/prax/mermaid` holds diagrams that the viewer draws beside mermaid.js (`tools/suite/mermaid/mermaid.html`),
  for a reader to compare.
