# Order fulfilment

How an order moves from checkout to the customer's door, and which service owns each step.

## The happy path

An order is accepted once payment is authorized and every item is reserved. Fulfilment then picks, packs and ships
from the nearest warehouse that holds all of the items.

```mermaid
flowchart LR
    checkout([Checkout]) --> pay{Payment<br>authorized?}
    pay -- yes --> reserve[Reserve stock]
    pay -- no --> declined([Declined])
    reserve --> pick[Pick and pack] --> ship[Ship] --> delivered([Delivered])
```

## Services

Each service owns its data, and talks to the others through the event bus. Only the gateway is reachable from
outside.

```mermaid
flowchart TB
    gateway[API gateway]
    subgraph core [Order core]
        orders[(Orders)]
        payments[Payments]
    end
    subgraph logistics [Logistics]
        stock[(Inventory)]
        shipping[Shipping]
    end
    gateway --> orders
    orders --> payments
    orders -. events .-> stock
    stock --> shipping
```

## Labels

Labels may use **Markdown strings**, which wrap, and HTML, which styles a word.

```mermaid
flowchart TD
    a["`**Retry** the payment up to three times, waiting longer before each attempt`"]
    b["Notify <b>support</b> and <i>hold</i> the order"]
    a --> b
```
