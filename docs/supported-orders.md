# Supported orders

The core constructs and inspects limit or market orders. A limit order has a positive integer tick price; a market order uses zero to indicate no limit price. The matching engine executes crossing limit and market orders at resting prices. Unfilled limits rest; unfilled market quantities are cancelled. The book accepts only limit orders and supports cancellation, modification and checked fills.

The book orders prices best first and preserves FIFO at each price, including after partial fills. A same-price quantity reduction (or no-op) retains priority; an increase or price change moves the order to the back of its destination level. Zero quantity is rejected, not treated as cancellation. Book maintenance operations do not yet implement exchange-level cancel/replace reports.
