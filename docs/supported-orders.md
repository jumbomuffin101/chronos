# Supported orders

Phase 1 can construct and inspect a limit or market order. A limit order has a positive integer tick price; a market order uses zero to indicate no limit price. Both start with positive original quantity, equal remaining quantity, and New status. There is no submission or execution.

Future work is intended to support limit orders resting or executing at eligible prices, market orders executing against available liquidity, cancellations, order modifications, and partial fills. The future book will apply price-time priority: better prices first, then earlier eligible orders at the same price. Details such as market-order residual handling, modification priority, and rejection rules remain to be specified and tested.
