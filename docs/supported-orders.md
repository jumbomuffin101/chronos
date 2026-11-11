# Supported orders

The core constructs and inspects limit or market orders. A limit order has a positive integer tick price; a market order uses zero to indicate no limit price. Both start with positive original quantity, equal remaining quantity, and New status. The in-memory book accepts only limit orders and supports cancellation and modification; it does not submit or execute them.

The book orders prices best first and preserves FIFO at each price. A same-price quantity reduction (or no-op) retains priority; an increase or price change moves the order to the back of its destination level. Zero quantity is rejected, not treated as cancellation. Execution, market-order handling and partial fills are future matching-engine work; residual and rejection policies remain to be specified.
