# Weather and dashboard refresh

The Weather panel at the top shows watched cities even when no strategy proposal exists.
Choose Today, Tomorrow or All upcoming, select a city, then use View ladder to inspect a contract.
Today follows the configured trading timezone (currently America/Chicago) and rolls forward at
local midnight. An event's date comes from its contract, not its closing timestamp.

Research only means the model is collecting estimates but is blocked from proposing a trade.
The reason is displayed beside the contract. It does not mean that weather data is missing.
Quote receipt time and model calculation time are shown separately; underlying weather sources
have their own collection intervals. No listed contracts, no forecast, stale data and feed errors
are displayed as different states.

The dashboard polls state and the watchlist about every three seconds and weather about every
six seconds while visible. Each job waits for its previous request to finish. Read requests time
out after ten seconds and retry automatically. Returning to a tab, reconnecting or choosing
Refresh now requests fresh data. The refresh button only reads dashboard data.

The status strip distinguishes a disconnected dashboard, a delayed market cycle and stale
account data. Each city also shows its own feed status. Expired cached markets are excluded from
the live watchlist; their rows and proposals remain available as historical records.

Proposal history is activity, not market discovery. Category and Today only filters apply to the
recent history; the history window contains the latest 60 proposals. Ticket inputs and bracket
drafts survive background polling. Strategy selectors are not replaced while being edited.

Validation: build/desk-tests and node tests/test_dashboard_refresh.js. CTest includes the
dashboard test when Node is available. Browser checks cover city/date selection, history filters,
ladder navigation, draft retention, loss of the server and automatic reconnection.
